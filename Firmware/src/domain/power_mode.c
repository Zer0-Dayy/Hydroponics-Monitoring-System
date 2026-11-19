#include "domain/power_mode.h"

#include <string.h>

enum {
  PHASE_BOOT = 0,
  PHASE_STEADY,     // one owner, nothing pending (Wi-Fi may still be pending in LOW: see NET_WAIT)
  PHASE_QUIESCING,  // waiting for the owner's RELEASED
  PHASE_NET_WAIT,   // low-power worker granted, waiting for Wi-Fi radio off
  PHASE_REPORT      // transitions disabled: report the level only
};

void powerModeInit(PowerModeFsm* f, const PowerModeConfig* cfg) {
  memset(f, 0, sizeof(*f));
  f->cfg = *cfg;
  f->mode = POWER_MODE_UNKNOWN;
  f->phase = PHASE_BOOT;
}

static void track(PowerModeFsm* f, const PowerModeInputs* in) {
  // A new epoch is an edge, even one this machine never sampled LOW.
  if (in->railEpoch != f->lastEpoch || in->edgesMissed) f->haveHigh = false;
  f->lastEpoch = in->railEpoch;
  if (in->externalPower) {
    f->haveLow = false;
    if (!f->haveHigh && !in->edgesMissed) {  // stability counts from the first watched tick
      f->haveHigh = true;
      f->highSinceMs = in->now;
    }
  } else {
    f->haveHigh = false;
    if (!f->haveLow) {
      f->haveLow = true;
      f->lowSinceMs = in->now;
    }
  }
}

static bool powerLost(const PowerModeFsm* f, MonoMs now) {
  return f->haveLow && now - f->lowSinceMs >= f->cfg.lossDebounceMs;
}

static bool powerStable(const PowerModeFsm* f, MonoMs now) {
  return f->haveHigh && now - f->highSinceMs >= f->cfg.returnStableMs;
}

static void restore(const PowerModeInputs* in, PowerModeActions* out) {
  out->restoreRail = true;
  out->restoreEpoch = in->railEpoch;
}

static void grant(PowerModeFsm* f, PowerWorker w, PowerModeActions* out) {
  ++f->gen;
  f->owner = w;
  out->grant = w;
  out->grantGen = f->gen;
}

static void startQuiesce(PowerModeFsm* f, PowerWorker target, MonoMs now, PowerModeActions* out) {
  out->quiesce = f->owner;
  out->quiesceGen = f->gen;
  f->target = target;
  f->ackDeadlineMs = now + f->cfg.ackTimeoutMs;
  f->phase = PHASE_QUIESCING;
}

static void enterLowPath(PowerModeFsm* f, MonoMs now, PowerModeActions* out) {
  out->wifiStop = true;
  f->netPending = true;
  f->netDeadlineMs = now + f->cfg.networkAckTimeoutMs;
  f->mode = POWER_MODE_TRANSITION_TO_LOW;
}

// The low-power worker holds the lease: LOW_POWER once Wi-Fi is off.
static void settleLow(PowerModeFsm* f, const PowerModeInputs* in) {
  if (f->netPending && in->networkOff) f->netPending = false;
  if (!f->netPending) {
    f->mode = POWER_MODE_LOW_POWER;
    f->phase = PHASE_STEADY;
  } else {
    f->phase = PHASE_NET_WAIT;
    if (in->now > f->netDeadlineMs) f->mode = POWER_MODE_DEGRADED;  // radio not confirmed off
  }
}

static void stepBoot(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  if (!f->cfg.transitionsEnabled) {
    grant(f, POWER_WORKER_NORMAL, out);
    if (!in->railValid) restore(in, out);
    f->mode = in->externalPower ? POWER_MODE_NORMAL : POWER_MODE_LOW_POWER;
    f->phase = PHASE_REPORT;
    return;
  }
  if (in->externalPower) {
    grant(f, POWER_WORKER_NORMAL, out);  // the rail is restored once power is stable
    f->mode = POWER_MODE_NORMAL;
    f->phase = PHASE_STEADY;
    return;
  }
  // Unknown or absent power at boot: conservative low-power policy (§13).
  grant(f, POWER_WORKER_LOW, out);
  enterLowPath(f, in->now, out);
  settleLow(f, in);
}

static void stepReport(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  if (!in->railValid) restore(in, out);  // no ISR in this mode; keep 5 V sensing on
  if (powerLost(f, in->now)) {
    f->mode = POWER_MODE_LOW_POWER;
  } else if (in->externalPower) {
    f->mode = POWER_MODE_NORMAL;
  }
}

static void stepSteady(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  if (f->owner == POWER_WORKER_NORMAL) {
    if (powerLost(f, in->now)) {
      enterLowPath(f, in->now, out);
      startQuiesce(f, POWER_WORKER_LOW, in->now, out);
    } else if (!in->railValid && powerStable(f, in->now)) {
      restore(in, out);  // a glitch the ISR saw, now stable again
    }
    return;
  }
  if (powerStable(f, in->now)) {  // owner LOW
    f->mode = POWER_MODE_TRANSITION_TO_NORMAL;
    startQuiesce(f, POWER_WORKER_NORMAL, in->now, out);
  }
}

static void stepQuiescing(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  // A new loss before the normal worker is granted aborts the return (§13).
  if (f->target == POWER_WORKER_NORMAL && !in->externalPower) {
    f->target = POWER_WORKER_LOW;
    f->mode = POWER_MODE_TRANSITION_TO_LOW;
    // Wi-Fi was never restarted on this path: it is still off.
  }
  if (in->releasedGen != f->gen) {
    if (in->now > f->ackDeadlineMs) f->mode = POWER_MODE_DEGRADED;  // keep waiting: never two owners
    return;
  }
  f->owner = POWER_WORKER_NONE;
  if (f->target == POWER_WORKER_NORMAL) {
    // A glitch during the hand-over restarted the timer: stepSteady restores
    // the rail once power has been stable again.
    if (powerStable(f, in->now)) restore(in, out);
    grant(f, POWER_WORKER_NORMAL, out);
    out->wifiStart = true;
    f->netPending = false;
    f->mode = POWER_MODE_NORMAL;
    f->phase = PHASE_STEADY;
    return;
  }
  grant(f, POWER_WORKER_LOW, out);
  f->mode = POWER_MODE_TRANSITION_TO_LOW;
  settleLow(f, in);
}

static void stepNetWait(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  if (powerStable(f, in->now)) {  // power back before Wi-Fi confirmed: return anyway
    f->netPending = false;
    f->mode = POWER_MODE_TRANSITION_TO_NORMAL;
    startQuiesce(f, POWER_WORKER_NORMAL, in->now, out);
    return;
  }
  settleLow(f, in);
}

void powerModeStep(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out) {
  memset(out, 0, sizeof(*out));
  track(f, in);
  switch (f->phase) {
    case PHASE_BOOT: stepBoot(f, in, out); break;
    case PHASE_REPORT: stepReport(f, in, out); break;
    case PHASE_STEADY: stepSteady(f, in, out); break;
    case PHASE_QUIESCING: stepQuiescing(f, in, out); break;
    case PHASE_NET_WAIT: stepNetWait(f, in, out); break;
    default: break;
  }
}

const char* powerModeName(PowerMode m) {
  switch (m) {
    case POWER_MODE_NORMAL: return "NORMAL";
    case POWER_MODE_TRANSITION_TO_LOW: return "TRANSITION_TO_LOW";
    case POWER_MODE_LOW_POWER: return "LOW_POWER";
    case POWER_MODE_TRANSITION_TO_NORMAL: return "TRANSITION_TO_NORMAL";
    case POWER_MODE_DEGRADED: return "DEGRADED";
    default: return "UNKNOWN";
  }
}

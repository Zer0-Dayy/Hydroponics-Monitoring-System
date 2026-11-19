// Power-mode state machine and acquisition lease decisions (§13, §14).
// Pure C, host-tested. Work package WP9 (Stage 6).
//
// PowerCoordinator (services/power_coordinator.c) feeds it the external-power
// sense level, the rail validity and the acknowledgements, once per tick, and
// executes the actions it returns. It never touches hardware itself.
//
//   NORMAL --(power low for lossDebounceMs)--> TRANSITION_TO_LOW:
//       quiesce the normal worker, stop Wi-Fi;
//       on RELEASED(gen): grant the low-power worker (3.3 V sensors only);
//       when Wi-Fi reports its radio off: LOW_POWER.
//   LOW_POWER --(power high for returnStableMs)--> TRANSITION_TO_NORMAL:
//       quiesce the low-power worker;
//       on RELEASED(gen): restore the 5 V rail, grant the normal worker (all
//       sensors), start Wi-Fi: NORMAL.
//       Power lost again before the grant aborts the return (back to the
//       low-power path).
//   A missing RELEASED after ackTimeoutMs, or Wi-Fi not off after
//   networkAckTimeoutMs, reports DEGRADED. The machine keeps waiting: it never
//   grants a second owner while the first has not released (§14), and moves
//   on as soon as the late acknowledgement arrives.
//
// Rail validity: the ISR invalidates the rail on every sense edge (also a
// short glitch). In NORMAL the machine restores it after returnStableMs of
// stable power. Stability is measured from the newest rail epoch: an edge the
// ISR saw but this machine did not sample (a LOW-HIGH pulse between two ticks)
// still restarts the timer, and so does a muted ISR (edges unobservable). The
// restore names the epoch it was decided on, so a newer edge is never
// acknowledged by mistake. Boot: power present -> normal worker, rail restored once
// stable; power absent -> conservative low-power path.
//
// transitionsEnabled = false (POLICY_POWER_TRANSITIONS_ENABLED 0): the normal
// worker keeps every sensor, the rail is kept valid and the mode only reports
// the debounced sense level (NORMAL / LOW_POWER), nothing is switched.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/types.h"

typedef uint8_t PowerMode;
enum {
  POWER_MODE_UNKNOWN = 0,
  POWER_MODE_NORMAL,
  POWER_MODE_TRANSITION_TO_LOW,
  POWER_MODE_LOW_POWER,
  POWER_MODE_TRANSITION_TO_NORMAL,
  POWER_MODE_DEGRADED
};

typedef uint8_t PowerWorker;
enum { POWER_WORKER_NONE = 0, POWER_WORKER_NORMAL, POWER_WORKER_LOW };

typedef struct PowerModeConfig {
  uint32_t lossDebounceMs;       // power low this long = lost (CFG_POWER_LOSS_DEBOUNCE_MS)
  uint32_t returnStableMs;       // power high this long = back (CFG_POWER_RETURN_STABLE_MS)
  uint32_t ackTimeoutMs;         // worker RELEASED deadline (CFG_TRANSITION_ACK_MS)
  uint32_t networkAckTimeoutMs;  // Wi-Fi radio off deadline (CFG_NETWORK_SHUTDOWN_ACK_MS)
  bool transitionsEnabled;
} PowerModeConfig;

typedef struct PowerModeInputs {
  MonoMs now;
  bool externalPower;     // sense level now (true = external 5 V present)
  bool railValid;         // rail_state validity now
  uint32_t releasedGen;   // newest generation the lease holder released (0 = none)
  bool networkOff;        // Wi-Fi acknowledged: radio off
  uint32_t railEpoch;     // rail_state epoch, read together with railValid
  bool edgesMissed;       // the sense ISR is muted: edges are not counted now
} PowerModeInputs;

// What PowerCoordinator must do after a step, in this order: quiesce,
// restore the rail, grant, Wi-Fi.
typedef struct PowerModeActions {
  PowerWorker quiesce;    // NONE = nothing
  uint32_t quiesceGen;
  bool restoreRail;
  uint32_t restoreEpoch;  // restore only if the rail is still at this epoch
  PowerWorker grant;      // NONE = nothing
  uint32_t grantGen;
  bool wifiStop;
  bool wifiStart;
} PowerModeActions;

typedef struct PowerModeFsm {
  PowerModeConfig cfg;
  PowerMode mode;
  uint8_t phase;          // private
  PowerWorker owner;      // lease holder (NONE while handing over)
  PowerWorker target;     // worker to grant once the owner has released
  uint32_t gen;           // generation of the current / pending lease
  bool netPending;        // waiting for Wi-Fi radio off
  bool haveLow, haveHigh;
  uint32_t lastEpoch;     // rail epoch seen on the previous step
  MonoMs lowSinceMs, highSinceMs;
  MonoMs ackDeadlineMs, netDeadlineMs;
} PowerModeFsm;

void powerModeInit(PowerModeFsm* f, const PowerModeConfig* cfg);
void powerModeStep(PowerModeFsm* f, const PowerModeInputs* in, PowerModeActions* out);

// Uppercase name ("NORMAL", ...). The contract sends it lowercased.
const char* powerModeName(PowerMode m);

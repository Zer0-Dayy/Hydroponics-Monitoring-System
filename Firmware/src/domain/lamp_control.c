#include "domain/lamp_control.h"

static void setState(LampControl* self, bool on, MonoMs nowMs) {
  if (on != self->on) {
    self->on = on;
    self->lastChangeMs = nowMs;
  }
  self->waiting = false;
}

void lampControlInit(LampControl* self, const LampControlConfig* config, MonoMs nowMs) {
  self->config = *config;
  self->on = false;
  self->waiting = false;
  self->lastChangeMs = nowMs;
  self->faults = 0;
}

void lampControlForceOff(LampControl* self, MonoMs nowMs) { setState(self, false, nowMs); }

bool lampControlUpdate(LampControl* self, const Reading* light, MonoMs nowMs) {
  const bool usable = light->health == HEALTH_OK || light->health == HEALTH_UNCALIBRATED;
  if (!usable) {
    if (self->faults < UINT8_MAX) ++self->faults;
    if (self->faults >= self->config.faultsToOff) setState(self, false, nowMs);
    return self->on;
  }
  self->faults = 0;

  // Thresholds are pin mV. After an on-device lux fit the value is lux, but
  // raw still carries the filtered pin mV: calibration never moves the lamp.
  const float levelMv = light->unit == UNIT_MILLIVOLT || !light->hasRaw ? light->value : (float)light->raw;
  const bool bright = (light->qualityFlags & kQualitySaturated) != 0;
  bool want = self->on;
  if (bright || levelMv > self->config.offAboveMv) {
    want = false;
  } else if (levelMv < self->config.onBelowMv) {
    want = true;
  }
  if (want == self->on) {
    self->waiting = false;
    return self->on;
  }

  const uint32_t dwellMs = self->on ? self->config.minOnMs : self->config.minOffMs;
  if (lampControlHeldMs(self, nowMs) >= dwellMs) {
    setState(self, want, nowMs);
  } else {
    self->waiting = true;
  }
  return self->on;
}

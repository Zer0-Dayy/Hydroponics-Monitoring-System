#include "drivers/light.h"

#include <math.h>

#include "domain/calibration.h"

static inline LightDriver* asLight(SensorDriver* d) { return (LightDriver*)d; }

static PollResult finish(LightDriver* self, MonoMs now);

static SensorId lightId(const SensorDriver* d) { (void)d; return SENSOR_LIGHT; }
static const char* lightName(const SensorDriver* d) { (void)d; return "dfr0026_light"; }
static uint8_t lightMetricCount(const SensorDriver* d) { (void)d; return 1; }

static DriverStatus lightInit(SensorDriver* d) {
  LightDriver* self = asLight(d);
  self->initialised_ = false;
  self->busy_ = false;
  if (self->config_.adc == 0) return DRIVER_FAULT;
  // Throwaway conversion: fails if the channel was never begun on an ADC1 pin,
  // and keeps any one-time ADC characterisation cost out of poll().
  uint32_t mv = 0;
  bool saturated = false;
  if (!adcChannelReadMilliVolts(self->config_.adc, &mv, &saturated)) return DRIVER_FAULT;
  self->initialised_ = true;
  return DRIVER_OK;
}

// Take one sample of the window at `now`. false on an ADC error.
static bool sample(LightDriver* self, MonoMs now) {
  uint32_t mv = 0;
  bool saturated = false;
  if (!adcChannelReadMilliVolts(self->config_.adc, &mv, &saturated)) return false;
  self->samples_[self->count_++] = mv;
  self->lastSampleMs_ = now;
  self->saturated_ = self->saturated_ || saturated;
  return true;
}

static DriverStatus lightStart(SensorDriver* d, MonoMs now) {
  LightDriver* self = asLight(d);
  if (!self->initialised_) return DRIVER_FAULT;
  self->count_ = 0;
  self->saturated_ = false;
  self->startMs_ = now;
  if (!sample(self, now)) {
    self->initialised_ = false;
    return DRIVER_FAULT;
  }
  self->busy_ = true;
  return DRIVER_OK;
}

static PollResult lightPoll(SensorDriver* d, MonoMs now) {
  LightDriver* self = asLight(d);
  PollResult r = {DRIVER_FAULT, now};
  if (!self->busy_) return r;

  // Spacing is measured from the previous actual sample, never from the window
  // start: after a late poll, catching up would compress the filter taps.
  const MonoMs due = self->lastSampleMs_ + LIGHT_SAMPLE_SPACING_MS;
  if (now < due) {
    r.status = DRIVER_PENDING;
    r.nextPollMs = due;
    return r;
  }
  if (!sample(self, now)) {
    self->busy_ = false;
    self->initialised_ = false;
    self->result_.health = HEALTH_FAULT;
    self->result_.intervalStartMs = self->startMs_;
    self->result_.intervalEndMs = now;
    return r;
  }
  if (self->count_ < LIGHT_SAMPLES) {
    r.status = DRIVER_PENDING;
    r.nextPollMs = self->lastSampleMs_ + LIGHT_SAMPLE_SPACING_MS;
    return r;
  }
  return finish(self, now);
}

static PollResult finish(LightDriver* self, MonoMs now) {
  self->busy_ = false;
  const float pinMv = trimmedMeanU32(self->samples_, LIGHT_SAMPLES, LIGHT_TRIM);

  Reading* r = &self->result_;
  r->intervalStartMs = self->startMs_;
  r->intervalEndMs = now;
  r->hasRaw = true;
  r->raw = (int32_t)(pinMv + 0.5f);
  // Clipped = brighter than the ADC can show: a lower bound, not a fault.
  r->qualityFlags = self->saturated_ ? kQualitySaturated : kQualityNone;
  r->calibrationRevision = self->config_.calibration.revision;
  if (self->config_.calibration.valid) {
    r->unit = UNIT_LUX;
    r->value = linearApply(&self->config_.calibration, pinMv);
    if (r->value < 0.0f) r->value = 0.0f;
    r->health = HEALTH_OK;
    if (!isfinite(r->value)) {  // a broken fit is a fault, never a value
      r->health = HEALTH_FAULT;
      PollResult bad = {DRIVER_READY, now};
      return bad;
    }
  } else {
    r->unit = UNIT_MILLIVOLT;
    r->value = pinMv;  // no divider: pin mV = module mV
    r->health = HEALTH_UNCALIBRATED;  // no lux mapping until a reference meter (D13)
  }
  r->lastGoodMs = now;  // last usable result (OK or UNCALIBRATED)
  ++r->sequence;
  PollResult pr = {DRIVER_READY, now};
  return pr;
}

static uint8_t lightReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  if (maxOut < 1) return 0;
  out[0] = asLight(d)->result_;
  return 1;
}

static void lightCancel(SensorDriver* d) {
  LightDriver* self = asLight(d);
  self->busy_ = false;
  self->count_ = 0;
}

// Retained rail, and the ADC pin is a high-impedance input behind 10 kOhm:
// nothing to drive. Drop any partial window.
static void lightPreparePowerOff(SensorDriver* d) { lightCancel(d); }

static const SensorDriverOps kLightOps = {
    .id = lightId,
    .name = lightName,
    .metricCount = lightMetricCount,
    .init = lightInit,
    .start = lightStart,
    .poll = lightPoll,
    .readResult = lightReadResult,
    .cancel = lightCancel,
    .preparePowerOff = lightPreparePowerOff,
};

void lightDriverConstruct(LightDriver* self, const LightConfig* config) {
  self->base.ops = &kLightOps;
  self->config_ = *config;
  self->initialised_ = false;
  self->busy_ = false;
  self->saturated_ = false;
  self->count_ = 0;
  self->startMs_ = 0;
  self->lastSampleMs_ = 0;
  readingReset(&self->result_, SENSOR_LIGHT, METRIC_LIGHT_LEVEL, UNIT_MILLIVOLT);
  self->result_.calibrationRevision = config->calibration.revision;
}

void lightDriverSetCalibration(LightDriver* self, const LinearCalibration* calibration) {
  self->config_.calibration = *calibration;
}

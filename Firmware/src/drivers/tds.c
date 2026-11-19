#include "drivers/tds.h"

#include <math.h>

#include "app_config.h"
#include "domain/tds_codec.h"

static inline TdsDriver* asTds(SensorDriver* d) { return (TdsDriver*)d; }

static PollResult finish(TdsDriver* self, MonoMs now);

static SensorId tdsId(const SensorDriver* d) { (void)d; return SENSOR_TDS; }
static const char* tdsName(const SensorDriver* d) { (void)d; return "sen0244_tds"; }
static uint8_t tdsMetricCount(const SensorDriver* d) { (void)d; return 1; }

static DriverStatus tdsInit(SensorDriver* d) {
  TdsDriver* self = asTds(d);
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
static bool sample(TdsDriver* self, MonoMs now) {
  uint32_t mv = 0;
  bool saturated = false;
  if (!adcChannelReadMilliVolts(self->config_.adc, &mv, &saturated)) return false;
  self->samples_[self->count_++] = mv;
  self->lastSampleMs_ = now;
  self->saturated_ = self->saturated_ || saturated;
  return true;
}

static DriverStatus tdsStart(SensorDriver* d, MonoMs now) {
  TdsDriver* self = asTds(d);
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

static PollResult tdsPoll(SensorDriver* d, MonoMs now) {
  TdsDriver* self = asTds(d);
  PollResult r = {DRIVER_FAULT, now};
  if (!self->busy_) return r;

  // Spacing is measured from the previous actual sample, never from the window
  // start: after a late poll, catching up would compress the filter taps.
  const MonoMs due = self->lastSampleMs_ + TDS_SAMPLE_SPACING_MS;
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
  if (self->count_ < TDS_SAMPLES) {
    r.status = DRIVER_PENDING;
    r.nextPollMs = self->lastSampleMs_ + TDS_SAMPLE_SPACING_MS;
    return r;
  }
  return finish(self, now);
}

// True if the stored water temperature can drive compensation at `now`.
static bool temperatureUsable(const TdsDriver* self, MonoMs now) {
  const Reading* t = &self->waterTemperature_;
  return readingIsActionableAt(t, now, CFG_STALE_ORDINARY_MS) &&
         t->value >= TDS_MIN_WATER_TEMP_C && t->value <= TDS_MAX_WATER_TEMP_C;
}

static PollResult finish(TdsDriver* self, MonoMs now) {
  self->busy_ = false;
  const TdsConfig* cfg = &self->config_;
  const float pinMv = trimmedMeanU32(self->samples_, TDS_SAMPLES, TDS_TRIM);  // = module mV

  const bool compensated = temperatureUsable(self, now);
  const float coefficient =
      compensated ? tdsCompensationCoefficient(self->waterTemperature_.value) : 1.0f;
  float ppm = tdsPpmFromModuleMv(pinMv, coefficient);
  if (cfg->calibration.valid) ppm = linearApply(&cfg->calibration, ppm);

  Reading* r = &self->result_;
  r->intervalStartMs = self->startMs_;
  r->intervalEndMs = now;
  r->hasRaw = true;
  r->raw = (int32_t)(pinMv + 0.5f);
  r->unit = UNIT_PPM;
  r->value = ppm;
  r->qualityFlags = compensated ? kQualityNone : kQualityUncompensated;
  r->calibrationRevision = cfg->calibration.revision;

  // READY in every case below: health carries the verdict, raw the level.
  PollResult pr = {DRIVER_READY, now};
  if (!isfinite(ppm)) {  // a broken fit is a fault, never a value
    r->health = HEALTH_FAULT;
    return pr;
  }
  if (self->saturated_ || ppm > TDS_FULL_SCALE_PPM) {
    r->qualityFlags |= kQualitySaturated;
    r->health = HEALTH_FAULT;  // above the rated 0-1000 ppm: never actionable
    return pr;
  }
  if (!compensated) {
    r->health = HEALTH_DEPENDENCY_INVALID;
    return pr;
  }
  ++r->sequence;
  r->lastGoodMs = now;  // last usable result (OK or UNCALIBRATED)
  if (!cfg->calibration.valid) {  // nominal datasheet curve, no reference check (D13)
    r->health = HEALTH_UNCALIBRATED;
    return pr;
  }
  r->health = HEALTH_OK;
  return pr;
}

static uint8_t tdsReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  if (maxOut < 1) return 0;
  out[0] = asTds(d)->result_;
  return 1;
}

static void tdsCancel(SensorDriver* d) {
  TdsDriver* self = asTds(d);
  self->busy_ = false;
  self->count_ = 0;
}

// The ADC pin is a high-impedance input (100 nF to GND): nothing to drive
// before the 5 V rail goes down. Drop any partial window.
static void tdsPreparePowerOff(SensorDriver* d) { tdsCancel(d); }

static const SensorDriverOps kTdsOps = {
    .id = tdsId,
    .name = tdsName,
    .metricCount = tdsMetricCount,
    .init = tdsInit,
    .start = tdsStart,
    .poll = tdsPoll,
    .readResult = tdsReadResult,
    .cancel = tdsCancel,
    .preparePowerOff = tdsPreparePowerOff,
};

void tdsDriverConstruct(TdsDriver* self, const TdsConfig* config) {
  self->base.ops = &kTdsOps;
  self->config_ = *config;
  self->initialised_ = false;
  self->busy_ = false;
  self->saturated_ = false;
  self->count_ = 0;
  self->startMs_ = 0;
  self->lastSampleMs_ = 0;
  readingReset(&self->waterTemperature_, SENSOR_DS18B20, METRIC_WATER_TEMPERATURE, UNIT_DEG_C);
  readingReset(&self->result_, SENSOR_TDS, METRIC_TDS, UNIT_PPM);
  self->result_.calibrationRevision = config->calibration.revision;
}

void tdsDriverSetWaterTemperature(TdsDriver* self, const Reading* waterTemperature) {
  self->waterTemperature_ = *waterTemperature;
}

void tdsDriverSetCalibration(TdsDriver* self, const LinearCalibration* calibration) {
  self->config_.calibration = *calibration;
}

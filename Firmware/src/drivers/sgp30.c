#include "drivers/sgp30.h"

#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "domain/sgp30_codec.h"
#include "hal/clock.h"

// Commands (datasheet Table 10).
static const uint16_t kCmdIaqInit = 0x2003;
static const uint16_t kCmdMeasureIaq = 0x2008;
static const uint16_t kCmdGetIaqBaseline = 0x2015;
static const uint16_t kCmdSetIaqBaseline = 0x201E;
static const uint16_t kCmdSetAbsoluteHumidity = 0x2061;
static const uint16_t kCmdGetFeatureSet = 0x202F;

static const uint16_t kProductTypeMask = 0xF000;  // Table 9: SGP30 = 0

// Timing. Driver-local because app_config.h is frozen.
static const uint32_t kCmdMaxMs = 10;           // iaq_init, baseline, humidity, feature set
static const uint32_t kFirstReadMs = 13;        // > measure_iaq max 12 ms, 1 ms clock margin
static const uint32_t kWarmupMs = 15000;        // fixed 400 ppm / 0 ppb after iaq_init
static const uint32_t kGapMs = 1800;            // start() later than this = missed second
static const uint8_t kMaxGaps = 3;              // consecutive gaps before a re-init
static const uint32_t kLearningMs = 12UL * 60 * 60 * 1000;  // no stored baseline
static const uint32_t kBaselineFetchPeriodMs = 60UL * 60 * 1000;
static const uint16_t kHumidityDeadband8p8 = 8;  // ~0.03 g/m3: ignore BME280 noise

enum {
  STEP_IDLE = 0,
  STEP_MEASURING,
  STEP_WRITE_FAILED,  // start() could not send the command; poll() reports it
  STEP_BASELINE_READ
};

static inline Sgp30Driver* asSgp30(SensorDriver* d) { return (Sgp30Driver*)d; }

static bool sendCommand(Sgp30Driver* self, uint16_t cmd);
static bool sendCommandWords(Sgp30Driver* self, uint16_t cmd, const uint16_t* words,
                             uint8_t nWords);
static bool readWords(Sgp30Driver* self, uint16_t* out, uint8_t nWords);
static PollResult fail(Sgp30Driver* self, MonoMs now);
static PollResult finish(Sgp30Driver* self, MonoMs now);
static void storeResults(Sgp30Driver* self, uint16_t co2eq, uint16_t tvoc, MonoMs now);

static SensorId sgp30Id(const SensorDriver* d) { (void)d; return SENSOR_SGP30; }
static const char* sgp30Name(const SensorDriver* d) { (void)d; return "sgp30"; }
static uint8_t sgp30MetricCount(const SensorDriver* d) { (void)d; return SGP30_METRICS; }

static DriverStatus sgp30Init(SensorDriver* d) {
  Sgp30Driver* self = asSgp30(d);
  self->initialised_ = false;
  self->step_ = STEP_IDLE;
  self->baselineStatus_ = SGP30_BASELINE_NONE;

  // Identity: feature set, product type 0. measure_test is not used (it must
  // not follow iaq_init and takes 220 ms), nor the General Call reset (it
  // would reset every device on the shared bus).
  uint16_t featureSet = 0;
  if (!sendCommand(self, kCmdGetFeatureSet)) {
    i2cBusRecover(self->config_.bus);
    return DRIVER_FAULT;
  }
  delay(kCmdMaxMs + 1);  // delay(n) may return up to 1 tick early
  if (!readWords(self, &featureSet, 1)) {
    i2cBusRecover(self->config_.bus);
    return DRIVER_FAULT;
  }
  if ((featureSet & kProductTypeMask) != 0) return DRIVER_NOT_PRESENT;
  self->featureSet_ = featureSet;

  if (!sendCommand(self, kCmdIaqInit)) return DRIVER_FAULT;
  delay(kCmdMaxMs + 1);  // delay(n) may return up to 1 tick early
  const MonoMs now = monoNowMs();

  // A stored baseline from Configuration wins over the RAM copy.
  if (self->hasPendingBaseline_) {
    self->baselineCo2eq_ = self->pendingCo2eq_;
    self->baselineTvoc_ = self->pendingTvoc_;
    self->hasBaseline_ = true;
    self->hasPendingBaseline_ = false;
  }
  if (self->hasBaseline_) {
    // set_iaq_baseline takes TVOC first, then CO2eq (datasheet §6.3).
    const uint16_t words[2] = {self->baselineTvoc_, self->baselineCo2eq_};
    if (!sendCommandWords(self, kCmdSetIaqBaseline, words, 2)) return DRIVER_FAULT;
    delay(kCmdMaxMs + 1);  // the harness may send measure_iaq right after init()
    self->baselineStatus_ = SGP30_BASELINE_RESTORED;
  } else {
    self->baselineStatus_ = SGP30_BASELINE_LEARNING;
    self->learningStartMs_ = now;
  }

  self->initMs_ = now;
  self->lastStartMs_ = now;
  self->baselineFetchDueMs_ = now + kBaselineFetchPeriodMs;
  self->consecutiveGaps_ = 0;
  self->humidityDirty_ = self->humidityEnabled_;  // re-send after a restart
  self->initialised_ = true;
  return DRIVER_OK;
}

static DriverStatus sgp30Start(SensorDriver* d, MonoMs now) {
  Sgp30Driver* self = asSgp30(d);
  if (!self->initialised_) return DRIVER_FAULT;

  if (now - self->lastStartMs_ > kGapMs && self->consecutiveGaps_ < 0xFF) {
    ++self->consecutiveGaps_;
  }
  self->lastStartMs_ = now;
  if (self->consecutiveGaps_ >= kMaxGaps) {
    self->initialised_ = false;  // caller re-runs init(): iaq_init + baseline
    return DRIVER_FAULT;
  }

  self->startMs_ = now;
  if (sendCommand(self, kCmdMeasureIaq)) {
    self->step_ = STEP_MEASURING;
  } else {
    // Reported by poll() as one gap, not as a re-init (that would cost 15 s).
    i2cBusRecover(self->config_.bus);
    self->step_ = STEP_WRITE_FAILED;
  }
  return DRIVER_OK;
}

static PollResult sgp30Poll(SensorDriver* d, MonoMs now) {
  Sgp30Driver* self = asSgp30(d);

  switch (self->step_) {
    case STEP_MEASURING: {
      if (now < self->startMs_ + kFirstReadMs) {
        PollResult r = {DRIVER_PENDING, self->startMs_ + kFirstReadMs};
        return r;
      }
      uint16_t words[2] = {0, 0};
      if (!readWords(self, words, 2)) {
        i2cBusRecover(self->config_.bus);
        return fail(self, now);
      }
      // measure_iaq answers CO2eq first, then TVOC (datasheet §6.3).
      storeResults(self, words[0], words[1], now);
      return finish(self, now);
    }

    case STEP_BASELINE_READ: {
      if (now < self->baselineReadyMs_) {
        PollResult r = {DRIVER_PENDING, self->baselineReadyMs_};
        return r;
      }
      // get_iaq_baseline answers CO2eq first, then TVOC. A failure only
      // delays the next copy: this second's readings are already stored.
      uint16_t words[2] = {0, 0};
      if (readWords(self, words, 2)) {
        self->baselineCo2eq_ = words[0];
        self->baselineTvoc_ = words[1];
        self->hasBaseline_ = true;
      }
      self->baselineFetchDueMs_ = now + kBaselineFetchPeriodMs;
      self->step_ = STEP_IDLE;
      PollResult r = {DRIVER_READY, now};
      return r;
    }

    case STEP_WRITE_FAILED:
      return fail(self, now);

    default: {
      // poll() without a start(): nothing in flight.
      PollResult r = {DRIVER_FAULT, now};
      return r;
    }
  }
}

// After a good measurement the bus is free and the next measure_iaq is ~1 s
// away: do at most one housekeeping command, so the chip is never asked for a
// second command while it is still processing the first (10 ms each). The
// hourly baseline copy goes first; a pending humidity write waits 1 s.
static PollResult finish(Sgp30Driver* self, MonoMs now) {
  self->step_ = STEP_IDLE;

  if (self->baselineStatus_ == SGP30_BASELINE_LEARNING &&
      now - self->learningStartMs_ >= kLearningMs) {
    self->baselineStatus_ = SGP30_BASELINE_VALID;
    self->baselineFetchDueMs_ = now;  // first copy right away
  }

  if ((self->baselineStatus_ == SGP30_BASELINE_VALID ||
       self->baselineStatus_ == SGP30_BASELINE_RESTORED) &&
      monoReached(now, self->baselineFetchDueMs_)) {
    if (sendCommand(self, kCmdGetIaqBaseline)) {
      self->step_ = STEP_BASELINE_READ;
      self->baselineReadyMs_ = now + kCmdMaxMs + 1;
      PollResult r = {DRIVER_PENDING, self->baselineReadyMs_};
      return r;
    }
    self->baselineFetchDueMs_ = now + kBaselineFetchPeriodMs;
  } else if (self->humidityDirty_) {
    const uint16_t word = self->humidity8p8_;
    if (sendCommandWords(self, kCmdSetAbsoluteHumidity, &word, 1)) {
      self->humidityDirty_ = false;
    }
  }

  PollResult r = {DRIVER_READY, now};
  return r;
}

static void storeResults(Sgp30Driver* self, uint16_t co2eq, uint16_t tvoc, MonoMs now) {
  self->consecutiveGaps_ = 0;

  // The caller stopped refreshing the humidity input: switch compensation off
  // on the chip (written at the next finish()) instead of keeping it silently.
  if (self->humidityEnabled_ && self->humidityValid_ &&
      now - self->humidityInputMs_ > CFG_STALE_BME280_MS) {
    self->humidityValid_ = false;
    self->humidity8p8_ = 0;
    self->humidityDirty_ = true;
  }

  Health health = HEALTH_OK;
  if (now - self->initMs_ < kWarmupMs) {
    health = HEALTH_WARMING;  // by time, never by the 400/0 values
  } else if (self->humidityEnabled_ && !self->humidityValid_) {
    health = HEALTH_DEPENDENCY_INVALID;
  } else if (self->baselineStatus_ == SGP30_BASELINE_LEARNING) {
    health = HEALTH_UNCALIBRATED;
  }
  const uint16_t quality = (self->humidityEnabled_ && self->humidityValid_)
                               ? kQualityNone
                               : kQualityUncompensated;

  const uint16_t raws[SGP30_METRICS] = {tvoc, co2eq};
  for (uint8_t i = 0; i < SGP30_METRICS; ++i) {
    Reading* r = &self->results_[i];
    r->intervalStartMs = self->startMs_;
    r->intervalEndMs = now;
    r->hasRaw = true;
    r->raw = raws[i];
    r->qualityFlags = quality;
    r->health = health;
    if (health == HEALTH_OK || health == HEALTH_UNCALIBRATED ||
        health == HEALTH_DEPENDENCY_INVALID) {
      r->value = (float)raws[i];
    }
    if (health == HEALTH_OK || health == HEALTH_UNCALIBRATED) {
      r->lastGoodMs = now;
      ++r->sequence;
    }
  }
}

// One rejected measurement: count the gap and mark both metrics FAULT. The
// driver stays initialised; start() re-inits only after kMaxGaps in a row.
static PollResult fail(Sgp30Driver* self, MonoMs now) {
  self->step_ = STEP_IDLE;
  if (self->consecutiveGaps_ < 0xFF) ++self->consecutiveGaps_;
  for (uint8_t i = 0; i < SGP30_METRICS; ++i) {
    self->results_[i].health = HEALTH_FAULT;
    self->results_[i].intervalStartMs = self->startMs_;
    self->results_[i].intervalEndMs = now;
  }
  PollResult r = {DRIVER_FAULT, now};
  return r;
}

static bool sendCommand(Sgp30Driver* self, uint16_t cmd) {
  const uint8_t tx[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
  return i2cBusWrite(self->config_.bus, self->config_.address, tx, sizeof(tx));
}

// Command followed by nWords (max 2) parameter words, each with its CRC.
static bool sendCommandWords(Sgp30Driver* self, uint16_t cmd, const uint16_t* words,
                             uint8_t nWords) {
  uint8_t tx[2 + 2 * 3];
  if (nWords > 2) return false;
  tx[0] = (uint8_t)(cmd >> 8);
  tx[1] = (uint8_t)(cmd & 0xFF);
  for (uint8_t i = 0; i < nWords; ++i) sgp30PackWord(words[i], &tx[2 + 3 * i]);
  return i2cBusWrite(self->config_.bus, self->config_.address, tx, 2 + 3 * (size_t)nWords);
}

static bool readWords(Sgp30Driver* self, uint16_t* out, uint8_t nWords) {
  uint8_t rx[2 * 3];
  if (nWords > 2) return false;
  const size_t len = 3 * (size_t)nWords;
  return i2cBusRead(self->config_.bus, self->config_.address, rx, len) &&
         sgp30UnpackWords(rx, nWords, out);
}

static uint8_t sgp30ReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  Sgp30Driver* self = asSgp30(d);
  if (out == NULL) return 0;
  const uint8_t n = maxOut < SGP30_METRICS ? maxOut : SGP30_METRICS;
  for (uint8_t i = 0; i < n; ++i) out[i] = self->results_[i];
  return n;
}

// A started measurement finishes by itself inside the chip.
static void sgp30Cancel(SensorDriver* d) { asSgp30(d)->step_ = STEP_IDLE; }

// The 3.3 V rail stays up in low power (§3); just require a fresh init(),
// which re-applies the RAM baseline.
static void sgp30PreparePowerOff(SensorDriver* d) {
  Sgp30Driver* self = asSgp30(d);
  self->step_ = STEP_IDLE;
  self->initialised_ = false;
}

static const SensorDriverOps kSgp30Ops = {
    .id = sgp30Id,
    .name = sgp30Name,
    .metricCount = sgp30MetricCount,
    .init = sgp30Init,
    .start = sgp30Start,
    .poll = sgp30Poll,
    .readResult = sgp30ReadResult,
    .cancel = sgp30Cancel,
    .preparePowerOff = sgp30PreparePowerOff,
};

void sgp30DriverSetHumidity(Sgp30Driver* self, const Reading* airTemperature,
                            const Reading* airHumidity, MonoMs now) {
  self->humidityEnabled_ = true;
  float grams = -1.0f;
  if (readingIsActionableAt(airTemperature, now, CFG_STALE_BME280_MS) &&
      readingIsActionableAt(airHumidity, now, CFG_STALE_BME280_MS)) {
    grams = sgp30AbsoluteHumidity(airTemperature->value, airHumidity->value);
  }
  // Stale or invalid input: write 0, which switches compensation off, rather
  // than let the chip keep an old value silently (§10).
  const uint16_t value = grams > 0.0f ? sgp30GramsToFixed8p8(grams) : 0;
  self->humidityValid_ = value != 0;
  if (self->humidityValid_) {
    const MonoMs t = airTemperature->intervalEndMs;
    const MonoMs h = airHumidity->intervalEndMs;
    self->humidityInputMs_ = t < h ? t : h;  // age of the older input
  }
  const uint16_t old = self->humidity8p8_;
  const uint16_t diff = value > old ? (uint16_t)(value - old) : (uint16_t)(old - value);
  if ((value == 0) != (old == 0) || diff >= kHumidityDeadband8p8) {
    self->humidity8p8_ = value;
    self->humidityDirty_ = true;
  }
}

void sgp30DriverSetBaseline(Sgp30Driver* self, uint16_t co2eq, uint16_t tvoc) {
  self->pendingCo2eq_ = co2eq;
  self->pendingTvoc_ = tvoc;
  self->hasPendingBaseline_ = true;
}

bool sgp30DriverGetBaseline(const Sgp30Driver* self, uint16_t* co2eq, uint16_t* tvoc) {
  const bool usable = self->hasBaseline_ &&
                      (self->baselineStatus_ == SGP30_BASELINE_VALID ||
                       self->baselineStatus_ == SGP30_BASELINE_RESTORED);
  if (usable) {
    if (co2eq != NULL) *co2eq = self->baselineCo2eq_;
    if (tvoc != NULL) *tvoc = self->baselineTvoc_;
  }
  return usable;
}

Sgp30BaselineStatus sgp30DriverBaselineStatus(const Sgp30Driver* self) {
  return self->baselineStatus_;
}

const char* sgp30BaselineStatusName(Sgp30BaselineStatus s) {
  switch (s) {
    case SGP30_BASELINE_NONE: return "NONE";
    case SGP30_BASELINE_LEARNING: return "LEARNING";
    case SGP30_BASELINE_RESTORED: return "RESTORED";
    case SGP30_BASELINE_VALID: return "VALID";
    default: return "?";
  }
}

uint16_t sgp30DriverFeatureSet(const Sgp30Driver* self) { return self->featureSet_; }

// Readings, baseline and humidity state are reset once here so they survive
// re-inits.
void sgp30DriverConstruct(Sgp30Driver* self, const Sgp30Config* config) {
  memset(self, 0, sizeof(*self));
  self->base.ops = &kSgp30Ops;
  self->config_ = *config;
  self->step_ = STEP_IDLE;
  self->baselineStatus_ = SGP30_BASELINE_NONE;
  readingReset(&self->results_[0], SENSOR_SGP30, METRIC_TVOC, UNIT_PPB);
  readingReset(&self->results_[1], SENSOR_SGP30, METRIC_CO2EQ, UNIT_PPM);
}

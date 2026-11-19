#include "drivers/bme280.h"

#include <Arduino.h>
#include <string.h>

#include "app_config.h"

// Registers (datasheet §5.3).
static const uint8_t kRegCalib00 = 0x88;  // ..0xA1, 26 bytes
static const uint8_t kRegChipId = 0xD0;
static const uint8_t kRegReset = 0xE0;
static const uint8_t kRegCalib26 = 0xE1;  // ..0xE7, 7 bytes
static const uint8_t kRegCtrlHum = 0xF2;
static const uint8_t kRegStatus = 0xF3;
static const uint8_t kRegCtrlMeas = 0xF4;
static const uint8_t kRegConfig = 0xF5;
static const uint8_t kRegData = 0xF7;     // ..0xFE, 8 bytes (P, T, H)

static const uint8_t kChipId = 0x60;
static const uint8_t kResetCmd = 0xB6;
static const uint8_t kStatusMeasuring = 1u << 3;
static const uint8_t kStatusImUpdate = 1u << 0;

static const uint8_t kCtrlHumX1 = 0x01;     // osrs_h = x1
static const uint8_t kConfigNoFilter = 0x00;  // IIR off, t_sb unused in forced mode
static const uint8_t kCtrlMeasForcedX1 = (1u << 5) | (1u << 2) | 0x01;  // 0x25

static const uint32_t kFirstPollMs = 10;     // > t_max 9.3 ms at x1/x1/x1 (§9.1)
static const uint8_t kResetWaitMaxMs = 10;   // bounded NVM-copy wait in init()

// Operating range (§1): -40..85 degC, 0..100 %RH, 300..1100 hPa.
static const int32_t kTminCenti = -4000, kTmaxCenti = 8500;
static const float kPminPa = 30000.0f, kPmaxPa = 110000.0f;

static inline uint16_t u16le(const uint8_t* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static inline int16_t s16le(const uint8_t* b) { return (int16_t)u16le(b); }

static inline Bme280Driver* asBme280(SensorDriver* d) { return (Bme280Driver*)d; }

static bool readCalibration(Bme280Driver* self);
static PollResult fail(Bme280Driver* self, DriverStatus status, Health health, MonoMs now);
static int32_t compensateT(Bme280Driver* self, int32_t adcT);   // 0.01 degC, sets tFine_
static uint32_t compensateP(Bme280Driver* self, int32_t adcP);  // Q24.8 Pa, 0 = invalid
static uint32_t compensateH(Bme280Driver* self, int32_t adcH);  // Q22.10 %RH

static SensorId bme280Id(const SensorDriver* d) { (void)d; return SENSOR_BME280; }
static const char* bme280Name(const SensorDriver* d) { (void)d; return "bme280"; }
static uint8_t bme280MetricCount(const SensorDriver* d) { (void)d; return BME280_METRICS; }

static DriverStatus bme280Init(SensorDriver* d) {
  Bme280Driver* self = asBme280(d);
  self->initialised_ = false;
  self->busy_ = false;
  I2cBus* bus = self->config_.bus;
  const uint8_t address = self->config_.address;

  uint8_t id = 0;
  if (!i2cBusReadRegs(bus, address, kRegChipId, &id, 1)) {
    i2cBusRecover(bus);
    return DRIVER_FAULT;
  }
  if (id != kChipId) return DRIVER_NOT_PRESENT;

  if (!i2cBusWriteReg(bus, address, kRegReset, kResetCmd)) return DRIVER_FAULT;
  // t_startup <= 2 ms (Table 1). delay(n) may return up to 1 tick early, so
  // a failed read is "not ready yet" until the bounded wait runs out.
  delay(3);
  bool ready = false;
  for (uint8_t i = 0; i < kResetWaitMaxMs && !ready; ++i) {
    uint8_t status = kStatusImUpdate;
    ready = i2cBusReadRegs(bus, address, kRegStatus, &status, 1) &&
            (status & kStatusImUpdate) == 0;
    if (!ready) delay(1);
  }
  if (!ready) return DRIVER_TIMEOUT;

  if (!readCalibration(self)) return DRIVER_FAULT;

  // Sleep mode after reset, so config writes are accepted (§5.4.6).
  // ctrl_hum takes effect on the ctrl_meas write in start() (§5.4.3).
  uint8_t check[2] = {0, 0};
  if (!i2cBusWriteReg(bus, address, kRegCtrlHum, kCtrlHumX1) ||
      !i2cBusWriteReg(bus, address, kRegConfig, kConfigNoFilter) ||
      !i2cBusReadRegs(bus, address, kRegCtrlHum, &check[0], 1) ||
      !i2cBusReadRegs(bus, address, kRegConfig, &check[1], 1)) {
    return DRIVER_FAULT;
  }
  if ((check[0] & 0x07) != kCtrlHumX1 || (check[1] & 0xFC) != kConfigNoFilter) {
    return DRIVER_FAULT;
  }

  self->initialised_ = true;
  return DRIVER_OK;
}

static bool readCalibration(Bme280Driver* self) {
  uint8_t a[26];
  uint8_t b[7];
  if (!i2cBusReadRegs(self->config_.bus, self->config_.address, kRegCalib00, a, sizeof(a)) ||
      !i2cBusReadRegs(self->config_.bus, self->config_.address, kRegCalib26, b, sizeof(b))) {
    return false;
  }
  Bme280Calib* c = &self->calib_;
  // Table 16. a[0] = 0x88, b[0] = 0xE1.
  c->t1 = u16le(&a[0]);
  c->t2 = s16le(&a[2]);
  c->t3 = s16le(&a[4]);
  c->p1 = u16le(&a[6]);
  c->p2 = s16le(&a[8]);
  c->p3 = s16le(&a[10]);
  c->p4 = s16le(&a[12]);
  c->p5 = s16le(&a[14]);
  c->p6 = s16le(&a[16]);
  c->p7 = s16le(&a[18]);
  c->p8 = s16le(&a[20]);
  c->p9 = s16le(&a[22]);
  c->h1 = a[25];                                   // 0xA1
  c->h2 = s16le(&b[0]);                            // 0xE1/0xE2
  c->h3 = b[2];                                    // 0xE3
  c->h4 = (int16_t)(((int8_t)b[3] * 16) | (b[4] & 0x0F));  // E4[11:4] E5[3:0]
  c->h5 = (int16_t)(((int8_t)b[5] * 16) | (b[4] >> 4));    // E6[11:4] E5[7:4]
  c->h6 = (int8_t)b[6];                            // 0xE7

  // Blank NVM or a bus returning 0x00/0xFF would make these 0/0xFFFF.
  return c->t1 != 0 && c->p1 != 0 && c->t1 != 0xFFFF && c->p1 != 0xFFFF;
}

static DriverStatus bme280Start(SensorDriver* d, MonoMs now) {
  Bme280Driver* self = asBme280(d);
  if (!self->initialised_) return DRIVER_FAULT;
  if (!i2cBusWriteReg(self->config_.bus, self->config_.address, kRegCtrlMeas,
                      kCtrlMeasForcedX1)) {
    self->initialised_ = false;
    return DRIVER_FAULT;
  }
  self->startMs_ = now;
  self->busy_ = true;
  return DRIVER_OK;
}

static PollResult bme280Poll(SensorDriver* d, MonoMs now) {
  Bme280Driver* self = asBme280(d);
  I2cBus* bus = self->config_.bus;
  const uint8_t address = self->config_.address;
  const MonoMs startMs = self->startMs_;

  if (!self->busy_) return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);

  // The measuring bit may not be set yet right after the trigger, so the
  // first status check waits until the maximum conversion time has passed.
  if (now < startMs + kFirstPollMs) {
    PollResult r = {DRIVER_PENDING, startMs + kFirstPollMs};
    return r;
  }

  // ctrl_hum + status in one burst. ctrl_hum back at its reset value means the
  // module was power-cycled and lost its settings: fail, so the caller re-inits.
  uint8_t cs[2] = {0, 0};
  if (!i2cBusReadRegs(bus, address, kRegCtrlHum, cs, sizeof(cs))) {
    i2cBusRecover(bus);
    return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);
  }
  if ((cs[0] & 0x07) != kCtrlHumX1) return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);
  const uint8_t status = cs[1];
  if (status & kStatusMeasuring) {
    if (now - startMs >= CFG_BME280_BUDGET_MS) {
      return fail(self, DRIVER_TIMEOUT, HEALTH_TIMEOUT, now);
    }
    PollResult r = {DRIVER_PENDING, now + 1};
    return r;
  }

  // One burst read: shadowing keeps P/T/H consistent only within it (§4.1).
  uint8_t dat[8];
  if (!i2cBusReadRegs(bus, address, kRegData, dat, sizeof(dat))) {
    i2cBusRecover(bus);
    return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);
  }
  self->busy_ = false;

  const int32_t adcP = ((int32_t)dat[0] << 12) | (dat[1] << 4) | (dat[2] >> 4);
  const int32_t adcT = ((int32_t)dat[3] << 12) | (dat[4] << 4) | (dat[5] >> 4);
  const int32_t adcH = ((int32_t)dat[6] << 8) | dat[7];

  // All-ones means a floating bus (impossible as real T/P data: at x1 the low
  // 4 bits are 0). 0x80000/0x8000 are NOT checked: at x1 they are valid
  // mid-scale readings (lost settings are detected via ctrl_hum above).
  if (adcT == 0xFFFFF || adcP == 0xFFFFF || adcH == 0xFFFF) {
    return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);
  }

  // Temperature first: it provides tFine_ for P and H.
  const int32_t tCenti = compensateT(self, adcT);
  if (tCenti < kTminCenti || tCenti > kTmaxCenti) {
    return fail(self, DRIVER_FAULT, HEALTH_FAULT, now);
  }
  const uint32_t pQ8 = compensateP(self, adcP);
  const uint32_t hQ10 = compensateH(self, adcH);

  const float values[BME280_METRICS] = {
      (float)tCenti / 100.0f,
      (float)hQ10 / 1024.0f,
      (float)pQ8 / 256.0f,
  };
  const int32_t raws[BME280_METRICS] = {adcT, adcH, adcP};
  const bool valid[BME280_METRICS] = {
      true,
      true,  // compensateH clamps to 0..100 %RH
      pQ8 != 0 && values[2] >= kPminPa && values[2] <= kPmaxPa,
  };

  for (uint8_t i = 0; i < BME280_METRICS; ++i) {
    Reading* r = &self->results_[i];
    r->intervalStartMs = startMs;
    r->intervalEndMs = now;
    r->hasRaw = true;
    r->raw = raws[i];
    r->qualityFlags = kQualityNone;
    if (valid[i]) {
      r->health = HEALTH_OK;
      r->value = values[i];
      r->lastGoodMs = now;
      ++r->sequence;
    } else {
      r->health = HEALTH_FAULT;
    }
  }

  PollResult r = {DRIVER_READY, now};
  return r;
}

// Abort the measurement, mark every metric with `health` and force a re-init
// on the next start(). The last value is kept but never reported as Ok.
static PollResult fail(Bme280Driver* self, DriverStatus status, Health health, MonoMs now) {
  self->busy_ = false;
  self->initialised_ = false;
  for (uint8_t i = 0; i < BME280_METRICS; ++i) {
    self->results_[i].health = health;
    self->results_[i].intervalStartMs = self->startMs_;
    self->results_[i].intervalEndMs = now;
  }
  PollResult r = {status, now};
  return r;
}

// Datasheet §4.2.3 (rev 1.1 reference code), fixed-point.
static int32_t compensateT(Bme280Driver* self, int32_t adcT) {
  const Bme280Calib* c = &self->calib_;
  const int32_t t1 = c->t1;
  int32_t var1 = ((((adcT >> 3) - (t1 << 1))) * (int32_t)c->t2) >> 11;
  int32_t var2 = (((((adcT >> 4) - t1) * ((adcT >> 4) - t1)) >> 12) *
                  (int32_t)c->t3) >> 14;
  self->tFine_ = var1 + var2;
  return (self->tFine_ * 5 + 128) >> 8;
}

static uint32_t compensateP(Bme280Driver* self, int32_t adcP) {
  const Bme280Calib* c = &self->calib_;
  int64_t var1 = (int64_t)self->tFine_ - 128000;
  int64_t var2 = var1 * var1 * c->p6;
  var2 = var2 + ((var1 * c->p5) * 131072);              // << 17
  var2 = var2 + ((int64_t)c->p4 * 34359738368LL);       // << 35
  var1 = ((var1 * var1 * c->p3) >> 8) + ((var1 * c->p2) * 4096);  // << 12
  var1 = (((int64_t)1 << 47) + var1) * c->p1 >> 33;
  if (var1 == 0) return 0;  // avoid division by zero
  int64_t p = 1048576 - adcP;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = ((int64_t)c->p9 * (p >> 13) * (p >> 13)) >> 25;
  var2 = ((int64_t)c->p8 * p) >> 19;
  p = ((p + var1 + var2) >> 8) + ((int64_t)c->p7 * 16);  // << 4; p7 is signed
  return (uint32_t)p;
}

static uint32_t compensateH(Bme280Driver* self, int32_t adcH) {
  const Bme280Calib* c = &self->calib_;
  int32_t v = self->tFine_ - 76800;
  // h4 is signed: multiply, not << 20 (a negative left shift is undefined in C).
  v = (((((adcH << 14) - ((int32_t)c->h4 * 1048576) -
          ((int32_t)c->h5 * v)) + 16384) >> 15) *
       (((((((v * (int32_t)c->h6) >> 10) *
            (((v * (int32_t)c->h3) >> 11) + 32768)) >> 10) + 2097152) *
             (int32_t)c->h2 + 8192) >> 14));
  v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->h1) >> 4);
  v = v < 0 ? 0 : v;
  v = v > 419430400 ? 419430400 : v;
  return (uint32_t)(v >> 12);
}

static uint8_t bme280ReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  Bme280Driver* self = asBme280(d);
  if (out == NULL) return 0;
  const uint8_t n = maxOut < BME280_METRICS ? maxOut : BME280_METRICS;
  for (uint8_t i = 0; i < n; ++i) out[i] = self->results_[i];
  return n;
}

// A forced conversion finishes by itself and the chip returns to sleep.
static void bme280Cancel(SensorDriver* d) { asBme280(d)->busy_ = false; }

// The 3.3 V rail stays up in low power (§3); just require a fresh init().
static void bme280PreparePowerOff(SensorDriver* d) {
  Bme280Driver* self = asBme280(d);
  self->busy_ = false;
  self->initialised_ = false;
}

static const SensorDriverOps kBme280Ops = {
    .id = bme280Id,
    .name = bme280Name,
    .metricCount = bme280MetricCount,
    .init = bme280Init,
    .start = bme280Start,
    .poll = bme280Poll,
    .readResult = bme280ReadResult,
    .cancel = bme280Cancel,
    .preparePowerOff = bme280PreparePowerOff,
};

// Readings are reset once here so sequence/lastGoodMs survive re-inits.
void bme280DriverConstruct(Bme280Driver* self, const Bme280Config* config) {
  self->base.ops = &kBme280Ops;
  self->config_ = *config;
  memset(&self->calib_, 0, sizeof(self->calib_));
  self->tFine_ = 0;
  self->startMs_ = 0;
  self->initialised_ = false;
  self->busy_ = false;
  readingReset(&self->results_[0], SENSOR_BME280, METRIC_AIR_TEMPERATURE, UNIT_DEG_C);
  readingReset(&self->results_[1], SENSOR_BME280, METRIC_AIR_HUMIDITY, UNIT_PERCENT_RH);
  readingReset(&self->results_[2], SENSOR_BME280, METRIC_AIR_PRESSURE, UNIT_PASCAL);
}

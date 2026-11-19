#include "drivers/ds18b20.h"

#include <string.h>

#include "app_config.h"
#include "domain/ds18b20_codec.h"

static const uint8_t kCmdConvertT = 0x44;
static const uint8_t kCmdReadScratchpad = 0xBE;
static const uint8_t kCmdWriteScratchpad = 0x4E;

static inline Ds18b20Driver* asDs18b20(SensorDriver* d) { return (Ds18b20Driver*)d; }

static void finish(Ds18b20Driver* self, const uint8_t sp[DS18B20_SCRATCHPAD_LEN], MonoMs now);

// No presence pulse: publish a FAULT verdict, as the other drivers do, so the
// last OK reading is not served as current. lastGoodMs keeps its value.
static void markNotPresent(Ds18b20Driver* self, MonoMs start, MonoMs now) {
  Reading* r = &self->result_;
  r->intervalStartMs = start;
  r->intervalEndMs = now;
  r->qualityFlags = kQualityNone;
  r->hasRaw = false;
  r->health = HEALTH_FAULT;
}

static SensorId ds18b20Id(const SensorDriver* d) { (void)d; return SENSOR_DS18B20; }
static const char* ds18b20Name(const SensorDriver* d) { (void)d; return "ds18b20"; }
static uint8_t ds18b20MetricCount(const SensorDriver* d) { (void)d; return 1; }

// Reset + Match ROM. false if no presence pulse.
static bool selectDevice(Ds18b20Driver* self) {
  OneWireBus* bus = self->config_.bus;
  if (!oneWireBusReset(bus)) return false;
  oneWireBusSelect(bus, self->rom_);
  return true;
}

// Read all 9 scratchpad bytes. false if no presence pulse; the content is
// checked by the caller.
static bool readScratchpad(Ds18b20Driver* self, uint8_t sp[DS18B20_SCRATCHPAD_LEN]) {
  if (!selectDevice(self)) return false;
  oneWireBusWriteByte(self->config_.bus, kCmdReadScratchpad);
  for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; ++i) {
    sp[i] = oneWireBusReadByte(self->config_.bus);
  }
  return true;
}

static DriverStatus ds18b20Init(SensorDriver* d) {
  Ds18b20Driver* self = asDs18b20(d);
  self->initialised_ = false;
  self->busy_ = false;
  if (self->config_.bus == 0) return DRIVER_FAULT;

  uint8_t rom[8];
  if (!oneWireBusSearchFirst(self->config_.bus, rom)) return DRIVER_NOT_PRESENT;
  if (!ds18b20RomValid(rom)) return DRIVER_NOT_PRESENT;  // not a DS18B20
  memcpy(self->rom_, rom, sizeof(rom));
  self->hasRom_ = true;

  uint8_t sp[DS18B20_SCRATCHPAD_LEN];
  if (!readScratchpad(self, sp)) return DRIVER_NOT_PRESENT;
  if (!ds18b20ScratchpadValid(sp)) return DRIVER_FAULT;
  if (!ds18b20Is12Bit(sp[4])) {
    // TH and TL are written back unchanged; the scratchpad only, not EEPROM.
    if (!selectDevice(self)) return DRIVER_NOT_PRESENT;
    oneWireBusWriteByte(self->config_.bus, kCmdWriteScratchpad);
    oneWireBusWriteByte(self->config_.bus, sp[2]);
    oneWireBusWriteByte(self->config_.bus, sp[3]);
    oneWireBusWriteByte(self->config_.bus, DS18B20_CONFIG_12BIT);
    if (!readScratchpad(self, sp)) return DRIVER_NOT_PRESENT;
    if (!ds18b20ScratchpadValid(sp) || !ds18b20Is12Bit(sp[4])) return DRIVER_FAULT;
  }
  self->initialised_ = true;
  return DRIVER_OK;
}

static DriverStatus ds18b20Start(SensorDriver* d, MonoMs now) {
  Ds18b20Driver* self = asDs18b20(d);
  if (!self->initialised_) return DRIVER_FAULT;
  if (!selectDevice(self)) {
    self->initialised_ = false;
    markNotPresent(self, now, now);
    return DRIVER_NOT_PRESENT;
  }
  oneWireBusWriteByte(self->config_.bus, kCmdConvertT);
  self->startMs_ = now;
  self->busy_ = true;
  return DRIVER_OK;
}

static PollResult ds18b20Poll(SensorDriver* d, MonoMs now) {
  Ds18b20Driver* self = asDs18b20(d);
  PollResult r = {DRIVER_FAULT, now};
  if (!self->busy_) return r;

  // Fixed worst-case conversion time: no bus traffic while converting.
  const MonoMs due = self->startMs_ + CFG_DS18B20_CONVERSION_MAX_MS;
  if (now < due) {
    r.status = DRIVER_PENDING;
    r.nextPollMs = due;
    return r;
  }
  self->busy_ = false;
  uint8_t sp[DS18B20_SCRATCHPAD_LEN];
  if (!readScratchpad(self, sp)) {
    self->initialised_ = false;
    markNotPresent(self, self->startMs_, now);
    r.status = DRIVER_NOT_PRESENT;
    return r;
  }
  finish(self, sp, now);
  r.status = DRIVER_READY;
  return r;
}

static void finish(Ds18b20Driver* self, const uint8_t sp[DS18B20_SCRATCHPAD_LEN], MonoMs now) {
  Reading* r = &self->result_;
  r->intervalStartMs = self->startMs_;
  r->intervalEndMs = now;
  r->qualityFlags = kQualityNone;
  r->hasRaw = false;

  // READY in every case below: health carries the verdict.
  if (!ds18b20ScratchpadValid(sp)) {
    r->health = HEALTH_FAULT;  // corrupted read; the content is not trusted
    return;
  }
  const int16_t raw = ds18b20RawTemperature(sp);
  r->hasRaw = true;
  r->raw = raw;
  if (raw == DS18B20_POWER_ON_RAW) {
    // The register still holds its reset value: the device restarted and did
    // not convert. Water never reads exactly +85 degC here.
    r->health = HEALTH_FAULT;
    self->initialised_ = false;  // re-apply the resolution
    return;
  }
  const float degC = ds18b20RawToDegC(raw);
  if (degC < DS18B20_MIN_C || degC > DS18B20_MAX_C) {
    r->health = HEALTH_FAULT;
    return;
  }
  r->value = degC;
  r->health = HEALTH_OK;
  r->lastGoodMs = now;
  ++r->sequence;
}

static uint8_t ds18b20ReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  if (maxOut < 1) return 0;
  out[0] = asDs18b20(d)->result_;
  return 1;
}

// A conversion in progress finishes on its own; its result is never read.
static void ds18b20Cancel(SensorDriver* d) { asDs18b20(d)->busy_ = false; }

// Retained rail, and the open-drain pin only ever releases the line between
// slots: nothing to drive. Drop any pending conversion.
static void ds18b20PreparePowerOff(SensorDriver* d) { ds18b20Cancel(d); }

static const SensorDriverOps kDs18b20Ops = {
    .id = ds18b20Id,
    .name = ds18b20Name,
    .metricCount = ds18b20MetricCount,
    .init = ds18b20Init,
    .start = ds18b20Start,
    .poll = ds18b20Poll,
    .readResult = ds18b20ReadResult,
    .cancel = ds18b20Cancel,
    .preparePowerOff = ds18b20PreparePowerOff,
};

void ds18b20DriverConstruct(Ds18b20Driver* self, const Ds18b20Config* config) {
  self->base.ops = &kDs18b20Ops;
  self->config_ = *config;
  self->initialised_ = false;
  self->busy_ = false;
  self->hasRom_ = false;
  memset(self->rom_, 0, sizeof(self->rom_));
  self->startMs_ = 0;
  readingReset(&self->result_, SENSOR_DS18B20, METRIC_WATER_TEMPERATURE, UNIT_DEG_C);
}

bool ds18b20DriverGetRom(const Ds18b20Driver* self, uint8_t rom[8]) {
  if (!self->hasRom_) return false;
  memcpy(rom, self->rom_, sizeof(self->rom_));
  return true;
}

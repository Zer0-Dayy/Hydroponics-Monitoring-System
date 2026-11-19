#include "drivers/hcsr04.h"

#include <stddef.h>

#include "domain/ultrasonic.h"

// Poll step while the echo is in flight. The width comes from the ISR's us
// timestamps, so this only sets how soon the result is noticed.
static const uint32_t kPollStepMs = 1;

static inline Hcsr04Driver* asHcsr04(SensorDriver* d) { return (Hcsr04Driver*)d; }

static PollResult finish(Hcsr04Driver* self, MonoMs now, Health health, bool hasRaw,
                         uint32_t rawUs, float mm);

static SensorId hcsr04Id(const SensorDriver* d) { (void)d; return SENSOR_HCSR04; }
static const char* hcsr04Name(const SensorDriver* d) { (void)d; return "hcsr04"; }
static uint8_t hcsr04MetricCount(const SensorDriver* d) { (void)d; return 1; }

static DriverStatus hcsr04Init(SensorDriver* d) {
  Hcsr04Driver* self = asHcsr04(d);
  self->initialised_ = false;
  self->busy_ = false;
  if (self->config_.capture == NULL || self->config_.timeoutMs == 0) return DRIVER_FAULT;
  pulseCaptureCancel(self->config_.capture);
  uint32_t width = 0;
  if (pulseCapturePoll(self->config_.capture, &width) == CAPTURE_ERROR) return DRIVER_FAULT;
  // Idle ECHO must be low; high means a stuck line or an echo still running.
  if (pulseCaptureEchoHigh(self->config_.capture)) return DRIVER_FAULT;
  self->initialised_ = true;
  return DRIVER_OK;
}

static DriverStatus hcsr04Start(SensorDriver* d, MonoMs now) {
  Hcsr04Driver* self = asHcsr04(d);
  if (!self->initialised_) return DRIVER_FAULT;
  self->startMs_ = now;
  if (!pulseCaptureTrigger(self->config_.capture)) {
    // ECHO high before the trigger: nothing was sent.
    self->initialised_ = false;
    finish(self, now, HEALTH_FAULT, false, 0, 0.0f);
    return DRIVER_FAULT;
  }
  self->busy_ = true;
  return DRIVER_OK;
}

static PollResult hcsr04Poll(SensorDriver* d, MonoMs now) {
  Hcsr04Driver* self = asHcsr04(d);
  PollResult r = {DRIVER_FAULT, now};
  if (!self->busy_) return r;
  PulseCapture* pc = self->config_.capture;

  uint32_t width = 0;
  const CaptureState s = pulseCapturePoll(pc, &width);
  if (s == CAPTURE_DONE) {
    // Accepted even on a late poll: the width is timestamped by the ISR.
    const float mm = ultrasonicEchoUsToMm(width, ULTRASONIC_NOMINAL_SPEED_MPS);
    return finish(self, now, ultrasonicInRangeMm(mm) ? HEALTH_OK : HEALTH_FAULT, true,
                  width, mm);
  }
  if (s != CAPTURE_WAITING && s != CAPTURE_HIGH) {  // capture lost its arming
    pulseCaptureCancel(pc);
    return finish(self, now, HEALTH_FAULT, false, 0, 0.0f);
  }

  const MonoMs deadline = self->startMs_ + self->config_.timeoutMs;
  if (!monoReached(now, deadline)) {
    MonoMs next = now + kPollStepMs;
    if (next > deadline) next = deadline;
    r.status = DRIVER_PENDING;
    r.nextPollMs = next;
    return r;
  }

  // Disarm first so a late falling edge is never credited to the next ping.
  const uint32_t elapsedUs = pulseCaptureElapsedUs(pc);
  pulseCaptureCancel(pc);
  if (s == CAPTURE_HIGH) return finish(self, now, HEALTH_TIMEOUT, true, elapsedUs, 0.0f);
  return finish(self, now, HEALTH_TIMEOUT, false, 0, 0.0f);  // never rose
}

// Store one outcome. Only HEALTH_OK carries a value; every other verdict keeps
// the previous value untouched and is reported through health, never as 0.
static PollResult finish(Hcsr04Driver* self, MonoMs now, Health health, bool hasRaw,
                         uint32_t rawUs, float mm) {
  self->busy_ = false;
  Reading* r = &self->result_;
  r->intervalStartMs = self->startMs_;
  r->intervalEndMs = now;
  r->hasRaw = hasRaw;
  r->raw = hasRaw ? (int32_t)rawUs : 0;
  r->qualityFlags = kQualityUncompensated;  // nominal 343 m/s, no air temperature
  r->unit = UNIT_MILLIMETRE;
  r->health = health;
  if (health == HEALTH_OK) {
    r->value = mm;
    r->lastGoodMs = now;
    ++r->sequence;
  }
  PollResult pr = {DRIVER_READY, now};
  return pr;
}

static uint8_t hcsr04ReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  if (out == NULL || maxOut < 1) return 0;
  out[0] = asHcsr04(d)->result_;
  return 1;
}

static void hcsr04Cancel(SensorDriver* d) {
  Hcsr04Driver* self = asHcsr04(d);
  self->busy_ = false;
  if (self->config_.capture != NULL) pulseCaptureCancel(self->config_.capture);
}

// TRIG low and ECHO a plain input before the 5 V rail goes (§12). init()
// again once the rail is back.
static void hcsr04PreparePowerOff(SensorDriver* d) {
  Hcsr04Driver* self = asHcsr04(d);
  hcsr04Cancel(d);
  if (self->config_.capture != NULL) pulseCaptureSetSafe(self->config_.capture);
  self->initialised_ = false;
}

static const SensorDriverOps kHcsr04Ops = {
    .id = hcsr04Id,
    .name = hcsr04Name,
    .metricCount = hcsr04MetricCount,
    .init = hcsr04Init,
    .start = hcsr04Start,
    .poll = hcsr04Poll,
    .readResult = hcsr04ReadResult,
    .cancel = hcsr04Cancel,
    .preparePowerOff = hcsr04PreparePowerOff,
};

void hcsr04DriverConstruct(Hcsr04Driver* self, const Hcsr04Config* config) {
  self->base.ops = &kHcsr04Ops;
  self->config_ = *config;
  self->initialised_ = false;
  self->busy_ = false;
  self->startMs_ = 0;
  readingReset(&self->result_, SENSOR_HCSR04, METRIC_WATER_DISTANCE, UNIT_MILLIMETRE);
}

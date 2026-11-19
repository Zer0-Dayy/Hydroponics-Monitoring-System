#include "hal/pulse_capture.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <stddef.h>

// ESP32-S3 GPIO numbers run 0-48.
static const int8_t kMaxGpio = 48;

static inline uint32_t nowUs(void) { return (uint32_t)esp_timer_get_time(); }

// Direct register read: IRAM-safe, unlike digitalRead().
static inline IRAM_ATTR bool echoLevel(int8_t pin) {
  if (pin < 32) return (REG_READ(GPIO_IN_REG) >> pin) & 1u;
  return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1u;
}

// Only records edges while armed. Rising edge -> HIGH; falling edge while HIGH
// -> DONE. The state is written after the timestamp, so a DONE seen by poll()
// always has a valid fallUs_.
static void IRAM_ATTR onEchoEdge(void* arg) {
  PulseCapture* pc = (PulseCapture*)arg;
  const uint32_t t = nowUs();
  const CaptureState s = pc->state_;
  if (s != CAPTURE_WAITING && s != CAPTURE_HIGH) return;
  if (echoLevel(pc->echo_)) {
    if (s == CAPTURE_WAITING) {
      pc->riseUs_ = t;
      pc->state_ = CAPTURE_HIGH;
    }
  } else if (s == CAPTURE_HIGH) {
    pc->fallUs_ = t;
    pc->state_ = CAPTURE_DONE;
  }
}

bool pulseCaptureBegin(PulseCapture* pc, int8_t trigPin, int8_t echoPin) {
  pc->begun_ = false;
  pc->state_ = CAPTURE_IDLE;
  pc->trig_ = trigPin;
  pc->echo_ = echoPin;
  if (trigPin < 0 || trigPin > kMaxGpio || echoPin < 0 || echoPin > kMaxGpio ||
      trigPin == echoPin) {
    return false;
  }
  digitalWrite((uint8_t)trigPin, LOW);
  pinMode((uint8_t)trigPin, OUTPUT);
  digitalWrite((uint8_t)trigPin, LOW);
  pinMode((uint8_t)echoPin, INPUT);  // no pull: no back-feeding the 5 V module
  attachInterruptArg((uint8_t)echoPin, onEchoEdge, pc, CHANGE);
  pc->begun_ = true;
  return true;
}

bool pulseCaptureTrigger(PulseCapture* pc) {
  if (!pc->begun_) return false;
  pc->state_ = CAPTURE_IDLE;  // ignore edges while re-arming
  if (echoLevel(pc->echo_)) return false;
  pc->riseUs_ = 0;
  pc->fallUs_ = 0;
  digitalWrite((uint8_t)pc->trig_, LOW);
  delayMicroseconds(2);
  pc->trigUs_ = nowUs();
  pc->state_ = CAPTURE_WAITING;
  digitalWrite((uint8_t)pc->trig_, HIGH);
  delayMicroseconds(10);  // datasheet: >= 10 us TTL pulse
  digitalWrite((uint8_t)pc->trig_, LOW);
  return true;
}

CaptureState pulseCapturePoll(PulseCapture* pc, uint32_t* widthUs) {
  if (widthUs) *widthUs = 0;
  if (!pc->begun_) return CAPTURE_ERROR;
  const CaptureState s = pc->state_;
  if (s == CAPTURE_DONE && widthUs) *widthUs = pc->fallUs_ - pc->riseUs_;
  return s;
}

uint32_t pulseCaptureElapsedUs(const PulseCapture* pc) {
  if (pc->trigUs_ == 0) return 0;
  return nowUs() - pc->trigUs_;
}

bool pulseCaptureEchoHigh(const PulseCapture* pc) {
  return pc->begun_ && echoLevel(pc->echo_);
}

void pulseCaptureCancel(PulseCapture* pc) { pc->state_ = CAPTURE_IDLE; }

// The interrupt stays attached (pinMode() keeps the edge type): while IDLE the
// ISR ignores edges, and the 2 kOhm divider leg holds ECHO low with the module
// unpowered. The capture is usable again as soon as the rail returns.
void pulseCaptureSetSafe(PulseCapture* pc) {
  pc->state_ = CAPTURE_IDLE;
  if (!pc->begun_) return;
  digitalWrite((uint8_t)pc->trig_, LOW);  // never drive TRIG high into an unpowered module
  pinMode((uint8_t)pc->trig_, OUTPUT);
  pinMode((uint8_t)pc->echo_, INPUT);  // no pulls
}

// Echo-pulse capture (HAL). Work package WP4 (with HC-SR04).
// Use RMT or an audited edge-capture ISR - not pulseIn() (§2, §7).
//
// Implementation: CHANGE interrupt on ECHO + esp_timer microsecond timestamps
// (decision WP4, Oct 2026). The ISR only records the two edges; the width is
// read by pulseCapturePoll() from task context. Timestamps are the low 32 bits
// of esp_timer_get_time(), so the ISR -> task handoff is single-word writes
// (no lock) and widths are a modulo-2^32 subtraction (valid for ~71 min).
// Accuracy does not depend on how often poll() is called. ISR latency jitter
// is a few us (< 1 mm of distance).
//
// The timeout policy belongs to the caller (driver budget): this HAL never
// gives up by itself. Cancel after a timeout so a late falling edge is never
// credited to the next trigger.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t CaptureState;
enum {
  CAPTURE_IDLE = 0,  // not armed (begun, cancelled, or nothing triggered)
  CAPTURE_WAITING,   // triggered, ECHO has not risen yet
  CAPTURE_DONE,      // rising and falling edge seen, width available
  CAPTURE_TIMEOUT,   // unused by this HAL (timeouts are the driver's), kept for callers
  CAPTURE_ERROR,     // not begun
  CAPTURE_HIGH       // ECHO rose, falling edge not seen yet
};

// Treat the members as private to pulse_capture.c. Initialise with PULSE_CAPTURE_INIT.
typedef struct PulseCapture {
  int8_t trig_;
  int8_t echo_;
  bool begun_;
  volatile CaptureState state_;  // written last by the ISR
  volatile uint32_t riseUs_;
  volatile uint32_t fallUs_;
  uint32_t trigUs_;
} PulseCapture;

#define PULSE_CAPTURE_INIT {-1, -1, false, CAPTURE_IDLE, 0, 0, 0}

// TRIG output low, ECHO input with NO pull (a pull-up would back-feed the
// unpowered 5 V module through the divider), CHANGE interrupt attached.
// false if a pin is invalid.
bool pulseCaptureBegin(PulseCapture* pc, int8_t trigPin, int8_t echoPin);

// 10 us trigger pulse, arm capture. Busy-waits ~12 us (2 us low + 10 us high).
// false (nothing sent) if not begun or ECHO is already high: a stuck line or a
// late echo from the previous ping.
bool pulseCaptureTrigger(PulseCapture* pc);

// Non-blocking. *widthUs = pulse width when DONE, else 0.
CaptureState pulseCapturePoll(PulseCapture* pc, uint32_t* widthUs);

// Microseconds since the last trigger (0 if never triggered).
uint32_t pulseCaptureElapsedUs(const PulseCapture* pc);

// Current ECHO level (false if not begun).
bool pulseCaptureEchoHigh(const PulseCapture* pc);

void pulseCaptureCancel(PulseCapture* pc);                     // disarm, back to IDLE
void pulseCaptureSetSafe(PulseCapture* pc);                     // TRIG low, ECHO input, no pulls (§12)

#include "hal/gpio_safe.h"

#include <Arduino.h>

#include "board_profile.h"

// Level first, then output: the pin never drives the active level, even for
// the instant between the two calls.
static void driveInactive(int8_t pin, bool activeLow) {
  if (!boardPinAssigned(pin)) return;
  const uint8_t level = activeLow ? HIGH : LOW;
  digitalWrite(pin, level);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
}

void gpioApplySafeDefaults(void) {
  // Relay 1 (GPIO15, active-high): de-energised = lamp OFF, lamp on the NO
  // contact. The 10 kOhm gate pull-down holds it off until here; from now on
  // only Safety changes it.
  driveInactive(BOARD_PIN_RELAY1, BOARD_RELAY1_ACTIVE_LOW);
  // HC-SR04 TRIG (GPIO17) would float until pulseCaptureBegin(). LOW never
  // back-feeds the module when its 5 V rail is off (§14).
  driveInactive(BOARD_PIN_HCSR04_TRIG, false);
}

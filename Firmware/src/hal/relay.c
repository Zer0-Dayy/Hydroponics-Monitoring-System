#include "hal/relay.h"

#include <Arduino.h>

#include "board_profile.h"

static bool s_on = false;  // gpioApplySafeDefaults() leaves it de-energised

void relayWrite(bool on) {
  if (!boardPinAssigned(BOARD_PIN_RELAY1)) return;
  const bool high = BOARD_RELAY1_ACTIVE_LOW ? !on : on;
  digitalWrite(BOARD_PIN_RELAY1, high ? HIGH : LOW);
  s_on = on;
}

bool relayIsOn(void) { return s_on; }

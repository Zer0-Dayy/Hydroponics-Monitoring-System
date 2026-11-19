// Relay 1 output (GPIO15, IRLZ44N low side). HAL, Stage 4 (WP7).
// SOLE USER: services/safety.c (§11 invariant "Safety alone writes relay
// outputs"). Do not include this header anywhere else.
//
// gpioApplySafeDefaults() has already driven the pin to its de-energised
// level and made it an output; relayWrite() only changes the level.
#pragma once
#include <stdbool.h>

// on = coil energised. Polarity from BOARD_RELAY1_ACTIVE_LOW.
void relayWrite(bool on);
bool relayIsOn(void);

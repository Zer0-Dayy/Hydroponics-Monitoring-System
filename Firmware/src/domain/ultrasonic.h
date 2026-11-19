// HC-SR04 echo-time conversion (WP4). Pure C, host-tested.
// Source: HC-SR04.pdf (module guide) p.2 specifications, p.4 equations 2-3.
//
// ECHO stays high for the sound's round trip, so distance = width * c / 2.
// With c = 343 m/s (20 degC, datasheet) that is 58.3 us per cm. No temperature
// compensation yet: c changes ~0.17 %/degC.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ULTRASONIC_NOMINAL_SPEED_MPS 343.0f

// Datasheet ranging distance 2-400 cm.
#define ULTRASONIC_MIN_MM 20.0f
#define ULTRASONIC_MAX_MM 4000.0f

// Echo pulse width (us) -> one-way distance (mm).
float ultrasonicEchoUsToMm(uint32_t widthUs, float speedMps);

// True inside the datasheet ranging distance (inclusive).
bool ultrasonicInRangeMm(float mm);

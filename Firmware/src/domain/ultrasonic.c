#include "domain/ultrasonic.h"

float ultrasonicEchoUsToMm(uint32_t widthUs, float speedMps) {
  // us * (m/s) = um; one way = half; um -> mm = /1000.
  return (float)widthUs * speedMps / 2000.0f;
}

bool ultrasonicInRangeMm(float mm) {
  return mm >= ULTRASONIC_MIN_MM && mm <= ULTRASONIC_MAX_MM;
}

// Raw calibration input from the latest snapshot (WP8, Calibration page).
// Pure C, host-tested.
//
// Acquisition stays the only ADC owner: a capture never touches a driver. It
// reads the raw level every calibratable driver already publishes in
// Reading.raw (filtered pin mV) and converts it to the x the stored fit uses:
//   pH     module mV   (pin mV through the 10k/15k divider)
//   TDS    ppm referred to 25 degC with the nominal datasheet curve, before K
//          (needs a fresh water temperature, as the driver does)
//   light  pin mV
#pragma once
#include <stdint.h>

#include "domain/reading.h"

typedef uint8_t CaptureStatus;
enum {
  CAPTURE_OK = 0,
  CAPTURE_STALE,           // no usable reading newer than maxAgeMs (also FAULT / UNPOWERED)
  CAPTURE_NO_TEMPERATURE,  // TDS only: water temperature missing, old or out of range
  CAPTURE_UNSUPPORTED      // not a calibratable sensor
};

typedef struct CaptureLimits {
  uint32_t maxAgeMs;            // the sensor's reading must be at most this old
  uint32_t temperatureMaxAgeMs; // TDS compensation input
  uint32_t phDividerTopOhm;
  uint32_t phDividerBottomOhm;
} CaptureLimits;

CaptureStatus calCaptureFromSnapshot(SensorId sensor, const Snapshot* snap, MonoMs now, const CaptureLimits* limits,
                                     float* xOut, uint32_t* ageMsOut);

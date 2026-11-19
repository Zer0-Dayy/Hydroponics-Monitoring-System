// Unit conversion and calibration stages (§3, §14). Pure C.
// Work packages: WP5 (pH/TDS), WP3 (light).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Recover the module output voltage from the ADC pin voltage through a
// resistor divider: Vmodule = Vadc * (Rtop + Rbottom) / Rbottom.
// Use MEASURED resistor values once available (§3).
float dividerRecoverMv(float adcMv, uint32_t rTopOhm, uint32_t rBottomOhm);

// Linear fit y = slope * x + offset, with a revision for traceability.
typedef struct LinearCalibration {
  float slope;
  float offset;
  uint16_t revision;
  bool valid;  // false => publish raw with HEALTH_UNCALIBRATED
} LinearCalibration;

float linearApply(const LinearCalibration* c, float x);

// True if slope and offset are finite numbers. Every fit maker below refuses
// non-finite input or output; config_blob refuses a stored non-finite fit.
bool linearCalibrationFinite(const LinearCalibration* c);

// Two-point fit through (x1, y1) and (x2, y2), e.g. module mV read in two pH
// buffers. false (and *out left invalid) if the x values are too close.
bool linearFromTwoPoints(float x1, float y1, float x2, float y2, uint16_t revision,
                         LinearCalibration* out);

// Sort `samples` in place, drop `trim` values at each end and return the mean
// of the rest (spike-rejecting filter, pH-4502C tutorial: 10 samples, mean of
// the middle 6). Returns 0 if n <= 2 * trim.
float trimmedMeanU32(uint32_t* samples, uint8_t n, uint8_t trim);

// ---- On-device calibration (WP8, Calibration page) ----------------------------
// Which stored fits the user made on the device (ConfigBlob.calOverridden).
// A fit without its bit is the compiled default and is refreshed at boot.
#define CAL_BIT_PH 0x01u
#define CAL_BIT_TDS 0x02u
#define CAL_BIT_LIGHT 0x04u
#define CAL_BITS_ALL 0x07u

// One-point gain through the origin, y = (y1 / x1) * x (TDS K-factor, light
// lux per mV). false if x1 or y1 is not positive or the gain is outside
// [minGain, maxGain].
bool linearGainFromPoint(float x1, float y1, float minGain, float maxGain, uint16_t revision,
                         LinearCalibration* out);

// Two-point pH buffer fit, rejected unless the slope (pH per module mV) is
// negative, |slope| is in [minAbsSlope, maxAbsSlope] and the buffers are at
// least 1 pH apart. A positive slope means the buffer values were swapped.
bool phFitFromBuffers(float mv1, float ph1, float mv2, float ph2, float minAbsSlope, float maxAbsSlope,
                      uint16_t revision, LinearCalibration* out);

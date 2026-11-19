#include "domain/calibration.h"

#include <math.h>

float dividerRecoverMv(float adcMv, uint32_t rTopOhm, uint32_t rBottomOhm) {
  if (rBottomOhm == 0) return 0.0f;
  return adcMv * (float)(rTopOhm + rBottomOhm) / (float)rBottomOhm;
}

float linearApply(const LinearCalibration* c, float x) {
  return c->slope * x + c->offset;
}

bool linearCalibrationFinite(const LinearCalibration* c) { return isfinite(c->slope) && isfinite(c->offset); }

bool linearFromTwoPoints(float x1, float y1, float x2, float y2, uint16_t revision,
                         LinearCalibration* out) {
  out->slope = 1.0f;
  out->offset = 0.0f;
  out->revision = revision;
  out->valid = false;
  if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2)) return false;
  const float dx = x2 - x1;
  if (!(dx <= -1e-3f || dx >= 1e-3f)) return false;
  const float slope = (y2 - y1) / dx;
  const float offset = y1 - slope * x1;
  if (!isfinite(slope) || !isfinite(offset)) return false;
  out->slope = slope;
  out->offset = offset;
  out->valid = true;
  return true;
}

float trimmedMeanU32(uint32_t* samples, uint8_t n, uint8_t trim) {
  if (n <= 2u * trim) return 0.0f;
  for (uint8_t i = 1; i < n; ++i) {  // insertion sort: n is ~10
    const uint32_t v = samples[i];
    uint8_t j = i;
    while (j > 0 && samples[j - 1] > v) {
      samples[j] = samples[j - 1];
      --j;
    }
    samples[j] = v;
  }
  uint64_t sum = 0;
  for (uint8_t i = trim; i < n - trim; ++i) sum += samples[i];
  return (float)sum / (float)(n - 2u * trim);
}

static void invalidFit(uint16_t revision, LinearCalibration* out) {
  out->slope = 1.0f;
  out->offset = 0.0f;
  out->revision = revision;
  out->valid = false;
}

bool linearGainFromPoint(float x1, float y1, float minGain, float maxGain, uint16_t revision,
                         LinearCalibration* out) {
  invalidFit(revision, out);
  if (!(x1 > 0.0f) || !(y1 > 0.0f) || !isfinite(x1) || !isfinite(y1)) return false;
  const float gain = y1 / x1;
  if (!isfinite(gain) || !(gain >= minGain && gain <= maxGain)) return false;
  out->slope = gain;
  out->valid = true;
  return true;
}

bool phFitFromBuffers(float mv1, float ph1, float mv2, float ph2, float minAbsSlope, float maxAbsSlope,
                      uint16_t revision, LinearCalibration* out) {
  invalidFit(revision, out);
  const float dph = ph2 - ph1;
  if (!(dph <= -1.0f || dph >= 1.0f)) return false;  // also NaN
  LinearCalibration fit;
  if (!linearFromTwoPoints(mv1, ph1, mv2, ph2, revision, &fit)) return false;
  // The pH-4502C output rises as pH falls: a positive slope means the two
  // buffers were swapped, and would read plausible but wrong pH as OK.
  if (!(fit.slope < 0.0f)) return false;
  const float mag = -fit.slope;
  if (!(mag >= minAbsSlope && mag <= maxAbsSlope)) return false;
  *out = fit;
  return true;
}

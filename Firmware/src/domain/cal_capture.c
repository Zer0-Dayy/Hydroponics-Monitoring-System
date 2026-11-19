#include "domain/cal_capture.h"

#include "domain/calibration.h"
#include "domain/tds_codec.h"

static bool usableRaw(const Reading* r, MonoMs now, uint32_t maxAgeMs, uint32_t* ageMs) {
  if (!r->hasRaw || r->intervalEndMs == 0) return false;
  if (r->health != HEALTH_OK && r->health != HEALTH_UNCALIBRATED) return false;
  const MonoMs age = now > r->intervalEndMs ? now - r->intervalEndMs : 0;
  if (age > maxAgeMs) return false;
  *ageMs = (uint32_t)age;
  return true;
}

CaptureStatus calCaptureFromSnapshot(SensorId sensor, const Snapshot* snap, MonoMs now, const CaptureLimits* limits,
                                     float* xOut, uint32_t* ageMsOut) {
  *xOut = 0.0f;
  *ageMsOut = 0;
  switch (sensor) {
    case SENSOR_PH: {
      const Reading* r = &snap->readings[METRIC_PH];
      if (!usableRaw(r, now, limits->maxAgeMs, ageMsOut)) return CAPTURE_STALE;
      *xOut = dividerRecoverMv((float)r->raw, limits->phDividerTopOhm, limits->phDividerBottomOhm);
      return CAPTURE_OK;
    }
    case SENSOR_LIGHT: {
      const Reading* r = &snap->readings[METRIC_LIGHT_LEVEL];
      if (!usableRaw(r, now, limits->maxAgeMs, ageMsOut)) return CAPTURE_STALE;
      *xOut = (float)r->raw;
      return CAPTURE_OK;
    }
    case SENSOR_TDS: {
      const Reading* t = &snap->readings[METRIC_WATER_TEMPERATURE];
      if (!readingIsActionableAt(t, now, limits->temperatureMaxAgeMs) || t->value < TDS_MIN_WATER_TEMP_C ||
          t->value > TDS_MAX_WATER_TEMP_C) {
        return CAPTURE_NO_TEMPERATURE;
      }
      const Reading* r = &snap->readings[METRIC_TDS];
      // DEPENDENCY_INVALID (no temperature at the driver) is not usable either.
      if (!usableRaw(r, now, limits->maxAgeMs, ageMsOut)) return CAPTURE_STALE;
      *xOut = tdsPpmFromModuleMv((float)r->raw, tdsCompensationCoefficient(t->value));
      return CAPTURE_OK;
    }
    default: return CAPTURE_UNSUPPORTED;
  }
}

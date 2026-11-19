#include "domain/tds_codec.h"

float tdsCompensationCoefficient(float waterTempC) {
  return 1.0f + 0.02f * (waterTempC - 25.0f);
}

float tdsPpmFromModuleMv(float moduleMv, float coefficient) {
  if (moduleMv <= 0.0f || coefficient <= 0.0f) return 0.0f;
  const float v = moduleMv / 1000.0f;
  const float ppm = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
  return ppm / coefficient;  // compensate the conductivity, not the voltage
}

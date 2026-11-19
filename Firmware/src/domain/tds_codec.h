// SEN0244 TDS conversion (WP5). Pure C, host-tested.
// Source: DFRobot Gravity Analog TDS Sensor SEN0244 (local copy in
// Electrical System Study/Datasheets/SEN0244.PDF) for the curve, and DFRobot's
// GravityTDS library for where the temperature compensation is applied:
//   ppm(T)      = (133.42 V^3 - 255.86 V^2 + 857.39 V) * 0.5
//   coefficient = 1 + 0.02 * (T - 25)
//   ppm25       = ppm(T) / coefficient
// The datasheet's sample sketch divides the VOLTAGE by the coefficient
// instead. Because the curve is not linear, that over-corrects: on 24 Oct 2026
// the same tap water at 11.4 / 33.6 / 11.6 degC gave 683 / 584 / 713 ppm with
// the sketch's method and 631 / 620 / 653 ppm compensating the ppm (EC), as
// the library does. Conductivity is what changes by ~2 %/degC.
// The sample's VREF/1024 scaling is not ported: the ADC HAL already returns
// calibrated millivolts, and the module output reaches the pin undivided.
#pragma once

#define TDS_FULL_SCALE_PPM 1000.0f   // rated range 0-1000 ppm, +/-10 % F.S.
#define TDS_MIN_WATER_TEMP_C 0.0f    // below: frozen water, not a usable input
#define TDS_MAX_WATER_TEMP_C 55.0f   // datasheet probe limit

// Temperature compensation coefficient, 1.0 at 25 degC.
float tdsCompensationCoefficient(float waterTempC);

// Module output (mV) to TDS (ppm) referred to 25 degC: the curve's ppm divided
// by `coefficient` (1.0 = no compensation). 0 for a non-positive voltage or
// coefficient.
float tdsPpmFromModuleMv(float moduleMv, float coefficient);

// Basic shared types. Domain layer: pure C, no Arduino/FreeRTOS includes.
//
// Enumerations are stored as uint8_t (the typedefs below) with the values in
// an anonymous enum. This keeps every record exactly the size it had with the
// C++ `enum class X : uint8_t` types (queue item sizes, RAM).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Monotonic milliseconds since boot. Used for every age and deadline.
// Never wall-clock/NTP time (§8). 64-bit so it does not wrap in practice.
typedef uint64_t MonoMs;

static inline bool monoReached(MonoMs now, MonoMs deadline) { return now >= deadline; }

typedef uint8_t SensorId;
enum {
  SENSOR_BME280 = 0,
  SENSOR_DS18B20,
  SENSOR_LIGHT,   // DFR0026
  SENSOR_HCSR04,
  SENSOR_PH,      // pH-4502C
  SENSOR_TDS,     // SEN0244
  SENSOR_SGP30,   // TVOC / CO2eq (replaced the MQ-7, D18)
  SENSOR_COUNT
};

// One entry per published metric. A sensor may produce several metrics.
typedef uint8_t MetricId;
enum {
  METRIC_AIR_TEMPERATURE = 0,  // BME280
  METRIC_AIR_HUMIDITY,         // BME280
  METRIC_AIR_PRESSURE,         // BME280
  METRIC_WATER_TEMPERATURE,    // DS18B20
  METRIC_LIGHT_LEVEL,          // DFR0026 (mV until calibrated, then lux)
  METRIC_WATER_DISTANCE,       // HC-SR04
  METRIC_PH,                   // pH-4502C
  METRIC_TDS,                  // SEN0244
  METRIC_TVOC,                 // SGP30, ppb
  METRIC_CO2EQ,                // SGP30, ppm (estimate, not measured CO2)
  METRIC_COUNT
};

typedef uint8_t Unit;
enum {
  UNIT_NONE = 0,
  UNIT_DEG_C,
  UNIT_PERCENT_RH,
  UNIT_PASCAL,
  UNIT_MILLIVOLT,
  UNIT_MILLIMETRE,
  UNIT_PH,
  UNIT_PPM,
  UNIT_ADC_RAW,
  UNIT_PPB,
  UNIT_LUX   // light after an on-device fit (WP8 calibration). Appended: values above unchanged
};

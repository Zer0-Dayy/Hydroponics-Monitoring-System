#include "domain/reading.h"

#include <string.h>

// The records must keep the layout they had in C++ (enum class : uint8_t):
// they are copied through queues and sized into RAM budgets.
_Static_assert(sizeof(Reading) == 56, "Reading layout changed");
// 528 -> 584 bytes on 22 Oct 2026: MQ-7 CO signal replaced by SGP30 TVOC + CO2eq.
_Static_assert(sizeof(Snapshot) == 584, "Snapshot layout changed");

void readingReset(Reading* r, SensorId sensor, MetricId metric, Unit unit) {
  memset(r, 0, sizeof(*r));
  r->sensor = sensor;
  r->metric = metric;
  r->unit = unit;
  r->health = HEALTH_STARTING;
  r->qualityFlags = kQualityNone;
  r->hasRaw = false;
}

bool readingIsActionable(const Reading* r) { return r->health == HEALTH_OK; }

Health readingHealthAt(const Reading* r, MonoMs now, uint32_t staleLimitMs) {
  if (r->health != HEALTH_OK) return r->health;
  if (now > r->intervalEndMs && (now - r->intervalEndMs) > staleLimitMs) {
    return HEALTH_STALE;
  }
  return HEALTH_OK;
}

bool readingIsActionableAt(const Reading* r, MonoMs now, uint32_t staleLimitMs) {
  return readingHealthAt(r, now, staleLimitMs) == HEALTH_OK;
}

const char* healthName(Health h) {
  switch (h) {
    case HEALTH_STARTING: return "STARTING";
    case HEALTH_WARMING: return "WARMING";
    case HEALTH_OK: return "OK";
    case HEALTH_STALE: return "STALE";
    case HEALTH_FAULT: return "FAULT";
    case HEALTH_TIMEOUT: return "TIMEOUT";
    case HEALTH_UNPOWERED: return "UNPOWERED";
    case HEALTH_UNCALIBRATED: return "UNCALIBRATED";
    case HEALTH_DEPENDENCY_INVALID: return "DEPENDENCY_INVALID";
  }
  return "?";
}

const char* metricName(MetricId m) {
  switch (m) {
    case METRIC_AIR_TEMPERATURE: return "air_temperature";
    case METRIC_AIR_HUMIDITY: return "air_humidity";
    case METRIC_AIR_PRESSURE: return "air_pressure";
    case METRIC_WATER_TEMPERATURE: return "water_temperature";
    case METRIC_LIGHT_LEVEL: return "light_level";
    case METRIC_WATER_DISTANCE: return "water_distance";
    case METRIC_PH: return "ph";
    case METRIC_TDS: return "tds";
    case METRIC_TVOC: return "tvoc";
    case METRIC_CO2EQ: return "co2eq";
    case METRIC_COUNT: break;
  }
  return "?";
}

const char* unitName(Unit u) {
  switch (u) {
    case UNIT_NONE: return "";
    case UNIT_DEG_C: return "degC";
    case UNIT_PERCENT_RH: return "%RH";
    case UNIT_PASCAL: return "Pa";
    case UNIT_MILLIVOLT: return "mV";
    case UNIT_MILLIMETRE: return "mm";
    case UNIT_PH: return "pH";
    case UNIT_PPM: return "ppm";
    case UNIT_ADC_RAW: return "raw";
    case UNIT_PPB: return "ppb";
    case UNIT_LUX: return "lux";
  }
  return "?";
}

static SensorId metricSensor(MetricId m) {
  switch (m) {
    case METRIC_AIR_TEMPERATURE:
    case METRIC_AIR_HUMIDITY:
    case METRIC_AIR_PRESSURE: return SENSOR_BME280;
    case METRIC_WATER_TEMPERATURE: return SENSOR_DS18B20;
    case METRIC_LIGHT_LEVEL: return SENSOR_LIGHT;
    case METRIC_WATER_DISTANCE: return SENSOR_HCSR04;
    case METRIC_PH: return SENSOR_PH;
    case METRIC_TDS: return SENSOR_TDS;
    case METRIC_TVOC:
    case METRIC_CO2EQ: return SENSOR_SGP30;
    case METRIC_COUNT: break;
  }
  return SENSOR_COUNT;
}

// The unit each driver publishes when calibrated. pH and light may publish
// mV while uncalibrated; the Reading's own unit field says which.
static Unit metricUnit(MetricId m) {
  switch (m) {
    case METRIC_AIR_TEMPERATURE: return UNIT_DEG_C;
    case METRIC_AIR_HUMIDITY: return UNIT_PERCENT_RH;
    case METRIC_AIR_PRESSURE: return UNIT_PASCAL;
    case METRIC_WATER_TEMPERATURE: return UNIT_DEG_C;
    case METRIC_LIGHT_LEVEL: return UNIT_MILLIVOLT;
    case METRIC_WATER_DISTANCE: return UNIT_MILLIMETRE;
    case METRIC_PH: return UNIT_PH;
    case METRIC_TDS: return UNIT_PPM;
    case METRIC_TVOC: return UNIT_PPB;
    case METRIC_CO2EQ: return UNIT_PPM;
    case METRIC_COUNT: break;
  }
  return UNIT_NONE;
}

void snapshotReset(Snapshot* s) {
  memset(s, 0, sizeof(*s));
  for (uint8_t i = 0; i < METRIC_COUNT; ++i) {
    readingReset(&s->readings[i], metricSensor((MetricId)i), (MetricId)i, metricUnit((MetricId)i));
  }
}

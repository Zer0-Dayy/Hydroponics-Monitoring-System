#include "domain/sensor_set.h"

#include <string.h>

typedef struct SensorInfo {
  const char* key;
  const char* label;
  const char* dependsNote;
} SensorInfo;

// Indexed by SensorId.
static const SensorInfo kSensors[SENSOR_COUNT] = {
    {"bme280", "BME280 air temperature, humidity, pressure",
     "SGP30 loses humidity compensation"},
    {"ds18b20", "DS18B20 water temperature", "TDS loses temperature compensation"},
    {"light", "DFR0026 light level", "Automatic lamp stays OFF"},
    {"hcsr04", "HC-SR04 water level", ""},
    {"ph", "pH-4502C solution pH", ""},
    {"tds", "SEN0244 TDS", ""},
    {"sgp30", "SGP30 TVOC / CO2eq", ""},
};

const char* sensorKey(SensorId s) { return s < SENSOR_COUNT ? kSensors[s].key : "?"; }

const char* sensorLabel(SensorId s) { return s < SENSOR_COUNT ? kSensors[s].label : "?"; }

const char* sensorDependsNote(SensorId s) {
  return s < SENSOR_COUNT ? kSensors[s].dependsNote : "";
}

SensorId sensorFromKey(const char* key, size_t len) {
  for (uint8_t i = 0; i < SENSOR_COUNT; ++i) {
    const char* k = kSensors[i].key;
    if (strlen(k) == len && memcmp(k, key, len) == 0) return (SensorId)i;
  }
  return SENSOR_COUNT;
}

void snapshotGateRail(Snapshot* s, bool railValid, uint32_t railEpoch) {
  for (uint8_t m = 0; m < METRIC_COUNT; ++m) {
    Reading* r = &s->readings[m];
    if (!sensorMaskHas(SENSOR_MASK_RAIL_5V, sensorOfMetric((MetricId)m))) continue;
    if (r->health == HEALTH_STARTING || r->health == HEALTH_UNPOWERED) continue;
    if (!railValid || r->powerEpoch != railEpoch) r->health = HEALTH_UNPOWERED;
  }
}

SensorId sensorOfMetric(MetricId m) {
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

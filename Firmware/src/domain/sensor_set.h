// Fixed sensor set as the app sees it (FirmwareContract.md v2, "Sensors").
// Pure C. Work package WP8 (Stage 5).
//
// Sensors are fixed by the board (pins in board_profile.h). The app can only
// enable or disable them; the enabled set is a SensorMask persisted by
// Configuration. Keys are the protocol names, stable across firmware versions.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/reading.h"
#include "domain/types.h"

typedef uint8_t SensorMask;  // bit per SensorId
#define SENSOR_MASK_ALL ((SensorMask)((1u << SENSOR_COUNT) - 1u))
// Sensors on the external, switched 5 V rail (§14), gated on the rail epoch.
#define SENSOR_MASK_RAIL_5V ((SensorMask)((1u << SENSOR_HCSR04) | (1u << SENSOR_PH) | (1u << SENSOR_TDS)))

static inline bool sensorMaskHas(SensorMask mask, SensorId s) {
  return s < SENSOR_COUNT && (mask & (1u << s)) != 0;
}

// Protocol key ("bme280", "ds18b20", "light", "hcsr04", "ph", "tds", "sgp30").
const char* sensorKey(SensorId s);
const char* sensorLabel(SensorId s);

// What disabling this sensor does to the others (app warning), "" if nothing.
const char* sensorDependsNote(SensorId s);

// SENSOR_COUNT if the key (len bytes, not NUL-terminated) is unknown.
SensorId sensorFromKey(const char* key, size_t len);

// Sensor that produces a metric; SENSOR_COUNT for an invalid metric.
SensorId sensorOfMetric(MetricId m);

// Consumer-side rail check (§12), on a snapshot copy: acquisition checks the
// epoch when it accepts a result, but the rail can fail after that. Every 5 V
// reading measured in another epoch than railEpoch, or any 5 V reading while
// the rail is not valid now, becomes UNPOWERED (never shown, captured or
// acted on). STARTING readings are left as they are.
void snapshotGateRail(Snapshot* s, bool railValid, uint32_t railEpoch);

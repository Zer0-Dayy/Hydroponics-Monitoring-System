// Reading contract and latest-snapshot record (Firmware Architecture §8).
// FROZEN CONTRACT - every driver, service and transport depends on it.
// Domain layer: pure C, no Arduino/FreeRTOS includes.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "domain/types.h"

typedef uint8_t Health;
enum {
  HEALTH_STARTING = 0,        // no valid result yet
  HEALTH_WARMING,             // heater/filter/first conversion settling
  HEALTH_OK,                  // fresh, valid, declared units/calibration
  HEALTH_STALE,               // last good value exceeded its age limit
  HEALTH_FAULT,               // transport, CRC, range or conversion error
  HEALTH_TIMEOUT,             // operation did not complete in its budget
  HEALTH_UNPOWERED,           // rail off or power epoch changed during measurement
  HEALTH_UNCALIBRATED,        // acquisition works, physical units not trustworthy
  HEALTH_DEPENDENCY_INVALID   // e.g. TDS without a valid water temperature
};

// Additional quality bits (combine with Health).
enum QualityFlag {
  kQualityNone = 0,
  kQualitySaturated = 1u << 0,     // ADC at/near rail
  kQualityUncompensated = 1u << 1, // compensation input missing
  kQualityFilterPartial = 1u << 2, // filter window not full
  kQualityLate = 1u << 3,          // completed after its deadline
};

// Fixed-size record, one per metric. Plain data: safe to copy through queues.
typedef struct Reading {
  SensorId sensor;
  MetricId metric;
  Unit unit;
  Health health;
  uint16_t qualityFlags;      // QualityFlag bits
  bool hasRaw;
  float value;                // in `unit`; meaningless unless health == Ok/Uncalibrated
  int32_t raw;                // ADC counts / mV / raw register value if hasRaw
  MonoMs intervalStartMs;     // acquisition interval
  MonoMs intervalEndMs;
  MonoMs lastGoodMs;          // last usable result (OK or UNCALIBRATED); 0 = never
  uint32_t sequence;          // per-metric sample counter
  uint32_t powerEpoch;        // rail epoch when measured (§12)
  uint16_t configRevision;
  uint16_t calibrationRevision;
} Reading;

// Latest-snapshot record published by the acquisition lease holder (§8, §9).
// A coherent copy, NOT a simultaneous measurement of all sensors.
typedef struct Snapshot {
  uint32_t frameSequence;
  MonoMs publishedMs;
  uint32_t powerEpoch;
  Reading readings[METRIC_COUNT];  // indexed by MetricId
} Snapshot;

// Initialise a record to STARTING with identity fields set.
void readingReset(Reading* r, SensorId sensor, MetricId metric, Unit unit);

// True if the stored health is OK. Does NOT check age: a consumer that acts on
// the value (Safety, control rules) must use readingIsActionableAt() instead.
bool readingIsActionable(const Reading* r);

// True only for fresh, valid, calibrated data (eligible for numeric rules):
// stored health OK and not older than staleLimitMs at `now` (§10).
bool readingIsActionableAt(const Reading* r, MonoMs now, uint32_t staleLimitMs);

// Returns Stale if an Ok reading is older than staleLimitMs at `now`,
// otherwise the reading's current health.
Health readingHealthAt(const Reading* r, MonoMs now, uint32_t staleLimitMs);

const char* healthName(Health h);
const char* metricName(MetricId m);
const char* unitName(Unit u);

void snapshotReset(Snapshot* s);

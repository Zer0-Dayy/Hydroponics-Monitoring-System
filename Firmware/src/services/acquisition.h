// Acquisition engine: the one serial owner of all sensor buses.
// Work package WP7 (Stage 4). Architecture §6, §7, §9, §12.
//
// Stage 4 (decision D15) had ONE worker, AcqNormal (core 1, priority 4), owning
// every driver and bus. It runs an absolute-deadline schedule (1 s frame +
// BME280 every 250 ms, domain/schedule), stamps each completed reading,
// evaluates the alarm rules for it, submits TRIP/CLEAR to Safety at once and
// publishes the snapshot after every completed reading and at each frame.
//
// WP9 (§14): two workers share the drivers and the schedule, and only the
// lease holder runs. AcqNormal (core 1) holds it with external power and runs
// every sensor; AcqLowPower (core 0) holds it in low power and runs only the
// 3.3 V sensors (ACQ_LOW_POWER_SENSORS). PowerCoordinator moves the lease:
//   acquisitionRequestQuiesce(gen)  the holder stops at its next pass
//                                   boundary, cancels pending steps, makes the
//                                   5 V interfaces safe if the rail is down
//                                   (preparePowerOff: HC-SR04 TRIG low), leaves
//                                   the task watchdog and releases;
//   acquisitionReleasedGeneration() the acknowledgement;
//   acquisitionGrant(worker, gen, allowed)  the next holder joins the
//                                   watchdog, restarts the schedule and applies
//                                   the configuration limited to `allowed`.
// A sensor outside `allowed` publishes UNPOWERED; when it comes back it is
// initialised again and shows WARMING until its first result.
// Rail gating (§14): a 5 V sensor (HC-SR04, pH, TDS) starts only while the
// rail is valid and its result is kept only if the rail is still valid with
// the same epoch at the end; otherwise it is published UNPOWERED.
// HALs begin in AcqNormal on core 1 (the GPIO ISR service is installed there).
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "domain/power_mode.h"
#include "domain/sensor_set.h"
#include "domain/types.h"

// Retained 3.3 V supply: the sensors that keep running in low power (§15).
#define ACQ_LOW_POWER_SENSORS                                                              \
  ((SensorMask)((1u << SENSOR_BME280) | (1u << SENSOR_DS18B20) | (1u << SENSOR_LIGHT) | \
                (1u << SENSOR_SGP30)))

bool acquisitionInit(void);  // construct drivers, create both workers; no lease yet

// Lease protocol, called only by PowerCoordinator (§14). Grant refuses (false)
// while another worker still holds the lease: never two owners.
bool acquisitionGrant(PowerWorker worker, uint32_t generation, SensorMask allowed);
bool acquisitionRequestQuiesce(uint32_t generation);
uint32_t acquisitionReleasedGeneration(void);  // 0 = nothing released yet

// True once AcqNormal has begun the buses (and the GPIO ISR service on core 1).
bool acquisitionHalsReady(void);

// Task id of the lease holder for the progress check; TASK_ID_COUNT if none.
TaskId acquisitionActiveTaskId(void);

// Counters for Supervisor (copied under a lock; zero-wait).
typedef struct AcquisitionStats {
  uint32_t frames;
  uint32_t frameLate;      // frame releases more than SCHEDULE_LATE_TOLERANCE_MS late
  uint32_t frameSkipped;
  uint32_t frameJitterMaxMs;
  uint32_t bmeLate;
  uint32_t bmeSkipped;
  uint32_t guardTimeouts;  // steps cancelled by the frame / slot guard
  uint32_t driverFailures; // start/poll results other than OK/PENDING/READY
  uint32_t ruleEvents;     // TRIP + CLEAR submitted to Safety
} AcquisitionStats;

void acquisitionGetStats(AcquisitionStats* out);

// SGP30 baseline status name ("none", "learning", "restored", "valid"),
// refreshed every frame. Zero wait, any task.
const char* acquisitionSgp30BaselineName(void);

// The lease holder's task (AcqNormal if none), e.g. to wake it for a new
// configuration. NULL before acquisitionInit().
TaskHandle_t acquisitionTaskHandle(void);
// For Supervisor's stack check.
TaskHandle_t acquisitionNormalTaskHandle(void);
TaskHandle_t acquisitionLowPowerTaskHandle(void);

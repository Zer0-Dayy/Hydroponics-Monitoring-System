// Common sensor driver contract (Firmware Architecture §5, §14).
// FROZEN CONTRACT - all seven drivers implement exactly this interface.
//
// Rules for every driver:
//  - Know hardware, report status. No MQTT/BLE/WiFi includes, no task
//    creation, no power-mode decisions, no relay changes.
//  - No hidden long delays. start() begins an operation; poll() returns
//    Pending + the next time it is worth polling again. The single
//    acquisition worker interleaves sensors using those deadlines.
//  - Worst-case blocking of every call must be documented and bounded.
//  - Configuration (bus, pins, address) is passed to the constructor
//    (<name>DriverConstruct()).
//
// C object model: a driver is a struct whose FIRST member is a SensorDriver.
// SensorDriver holds a pointer to the driver's const operations table (the
// "vtable"). Callers use the sensorDriver*() functions below on a
// SensorDriver*, which dispatch through that table; the driver casts the
// SensorDriver* back to its own type.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "domain/reading.h"

typedef uint8_t DriverStatus;
enum {
  DRIVER_OK = 0,          // call accepted (init/start)
  DRIVER_PENDING,         // operation in progress, poll again at nextPollMs
  DRIVER_READY,           // result available via readResult()
  DRIVER_NOT_PRESENT,     // device missing / wrong identity
  DRIVER_FAULT,           // bus, CRC or range error
  DRIVER_TIMEOUT,         // exceeded its budget
  DRIVER_UNPOWERED,       // supply rail not available
  DRIVER_NOT_IMPLEMENTED  // stub (Stage 1 skeleton)
};

typedef struct PollResult {
  DriverStatus status;
  MonoMs nextPollMs;  // meaningful when status == DRIVER_PENDING
} PollResult;

typedef struct SensorDriver SensorDriver;

typedef struct SensorDriverOps {
  SensorId (*id)(const SensorDriver* self);
  const char* (*name)(const SensorDriver* self);

  // Number of Reading records this driver produces per result.
  uint8_t (*metricCount)(const SensorDriver* self);

  // Probe and configure the device. May be called again after a fault.
  DriverStatus (*init)(SensorDriver* self);

  // Begin one measurement (e.g. trigger a conversion). Must not block on it.
  DriverStatus (*start)(SensorDriver* self, MonoMs now);

  // Advance the measurement. Never blocks beyond the documented budget.
  PollResult (*poll)(SensorDriver* self, MonoMs now);

  // Copy up to maxOut readings of the last completed measurement.
  // Returns the number written. Health/quality are filled by the driver;
  // powerEpoch/configRevision are stamped by the acquisition service.
  uint8_t (*readResult)(SensorDriver* self, Reading* out, uint8_t maxOut);

  // Abandon any pending operation (deadline miss, quiesce request).
  void (*cancel)(SensorDriver* self);

  // Put the interface pins into a safe state before the rail is removed
  // (§12: no back-feeding an unpowered module).
  void (*preparePowerOff)(SensorDriver* self);
} SensorDriverOps;

struct SensorDriver {
  const SensorDriverOps* ops;
};

static inline SensorId sensorDriverId(const SensorDriver* d) { return d->ops->id(d); }
static inline const char* sensorDriverName(const SensorDriver* d) { return d->ops->name(d); }
static inline uint8_t sensorDriverMetricCount(const SensorDriver* d) {
  return d->ops->metricCount(d);
}
static inline DriverStatus sensorDriverInit(SensorDriver* d) { return d->ops->init(d); }
static inline DriverStatus sensorDriverStart(SensorDriver* d, MonoMs now) {
  return d->ops->start(d, now);
}
static inline PollResult sensorDriverPoll(SensorDriver* d, MonoMs now) {
  return d->ops->poll(d, now);
}
static inline uint8_t sensorDriverReadResult(SensorDriver* d, Reading* out, uint8_t maxOut) {
  return d->ops->readResult(d, out, maxOut);
}
static inline void sensorDriverCancel(SensorDriver* d) { d->ops->cancel(d); }
static inline void sensorDriverPreparePowerOff(SensorDriver* d) { d->ops->preparePowerOff(d); }

const char* driverStatusName(DriverStatus s);

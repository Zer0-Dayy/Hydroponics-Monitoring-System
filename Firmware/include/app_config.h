// Application timing, queue and task budgets.
// Source: Firmware Architecture §6 (tasks), §7 (schedule), §8 (staleness),
// §9 (queues), §11 (power). These are DESIGN BUDGETS, not measurements:
// replace with measured values as each stage is validated.
//
// Pure C (no Arduino headers).
#pragma once
#include <stdint.h>

// ---- Schedule (§7) -----------------------------------------------------------
#define CFG_FRAME_PERIOD_MS ((uint32_t)1000)         // ordinary sensors
#define CFG_BME280_PERIOD_MS ((uint32_t)250)         // fast climate check
#define CFG_BME280_BUDGET_MS ((uint32_t)12)          // conversion + read
#define CFG_DS18B20_CONVERSION_MAX_MS ((uint32_t)750)  // 12-bit
#define CFG_DS18B20_BUDGET_MS ((uint32_t)20)
#define CFG_HCSR04_TIMEOUT_MS ((uint32_t)30)
#define CFG_ANALOG_GROUP_BUDGET_MS ((uint32_t)5)
#define CFG_SNAPSHOT_BUDGET_MS ((uint32_t)10)
#define CFG_FRAME_RESERVATION_MS ((uint32_t)150)
#define CFG_SERVICE_STEP_TARGET_MS ((uint32_t)5)

// ---- Staleness (§8) ------------------------------------------------------------
#define CFG_STALE_BME280_MS ((uint32_t)1000)
#define CFG_STALE_ORDINARY_MS ((uint32_t)3000)

// ---- Queues (§9) ---------------------------------------------------------------
#define CFG_ALARM_DETAIL_QUEUE_LEN ((uint32_t)16)
#define CFG_ALARM_JOURNAL_LEN ((uint32_t)64)
#define CFG_COMMAND_QUEUE_LEN ((uint32_t)8)
#define CFG_TELEMETRY_BACKLOG_LEN ((uint32_t)60)
#define CFG_LOG_QUEUE_LEN ((uint32_t)32)

// ---- Power transitions (§11) ---------------------------------------------------
#define CFG_POWER_LOSS_DEBOUNCE_MS ((uint32_t)50)
#define CFG_POWER_RETURN_STABLE_MS ((uint32_t)500)
#define CFG_TRANSITION_ACK_MS ((uint32_t)200)
#define CFG_NETWORK_SHUTDOWN_ACK_MS ((uint32_t)2000)

// ---- Network (§15) -------------------------------------------------------------
#define CFG_MQTT_BACKOFF_INITIAL_MS ((uint32_t)1000)
#define CFG_MQTT_BACKOFF_MAX_MS ((uint32_t)60000)
#define CFG_BLE_STATE_PERIOD_MS ((uint32_t)2000)
#define CFG_BLE_TELEMETRY_PERIOD_MS ((uint32_t)3000)

// ---- Safety / supervision (§10, §16) -------------------------------------------
#define CFG_SAFETY_HEALTH_CHECK_MS ((uint32_t)100)
#define CFG_SUPERVISOR_PERIOD_MS ((uint32_t)1000)
#define CFG_TASK_WATCHDOG_MS ((uint32_t)5000)

// ---- Tasks (§6). Higher number = higher priority. Stack in bytes. --------------
typedef uint8_t TaskId;
enum {
  TASK_ID_SAFETY = 0,
  TASK_ID_POWER_COORDINATOR,
  TASK_ID_ACQUISITION_NORMAL,
  TASK_ID_ACQUISITION_LOW_POWER,
  TASK_ID_WIFI_MQTT,
  TASK_ID_BLE_TRANSPORT,
  TASK_ID_CONFIGURATION,
  TASK_ID_SUPERVISOR,
  TASK_ID_SERIAL_LOGGER,
  TASK_ID_COUNT
};

typedef struct TaskSpec {
  const char* name;
  uint8_t core;
  uint8_t priority;
  uint32_t stackBytes;
} TaskSpec;

static inline const TaskSpec* cfgTaskSpec(TaskId id) {
  // Indexed by TaskId. Keep order in sync with the enum above.
  static const TaskSpec kTaskSpecs[TASK_ID_COUNT] = {
      // Core 1 since 28 Oct 2026 (D14 fallback): the Wi-Fi driver's start-up
      // starved Safety on core 0 for 40-110 ms; on core 1 the worst case is
      // 145 us. Frozen-header change made on the project owner's decision.
      {"Safety", 1, 6, 3 * 1024},
      {"PowerCoord", 0, 5, 4 * 1024},
      {"AcqNormal", 1, 4, 4 * 1024},
      {"AcqLowPower", 0, 4, 4 * 1024},
      {"WifiMqtt", 0, 2, 6 * 1024},
      {"BleTransport", 0, 2, 6 * 1024},
      {"Configuration", 0, 2, 4 * 1024},
      {"Supervisor", 0, 1, 3 * 1024},
      {"SerialLogger", 1, 1, 2 * 1024},
  };
  return &kTaskSpecs[id];
}

// ---- CPU frequency (§13) - 80 MHz low power is only a candidate --------------------
#define CFG_CPU_MHZ_NORMAL ((uint32_t)160)

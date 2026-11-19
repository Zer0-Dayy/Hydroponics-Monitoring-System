#include "services/acquisition.h"

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"
#include "board_profile.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "domain/alarm.h"
#include "domain/schedule.h"
#include "drivers/bme280.h"
#include "drivers/ds18b20.h"
#include "drivers/hcsr04.h"
#include "drivers/light.h"
#include "drivers/ph.h"
#include "drivers/sgp30.h"
#include "drivers/tds.h"
#include "hal/adc_channel.h"
#include "hal/clock.h"
#include "hal/i2c_bus.h"
#include "hal/onewire_bus.h"
#include "hal/pulse_capture.h"
#include "policy_config.h"
#include "services/configuration.h"
#include "services/flash_gate.h"
#include "services/rail_state.h"
#include "services/safety.h"
#include "services/snapshot_store.h"

// ---- Buses and drivers: owned by the lease holder only (§11, §14) ------------
static I2cBus s_i2c = I2C_BUS_INIT;
static OneWireBus s_oneWire = ONE_WIRE_BUS_INIT;
static AdcChannel s_adcLight = ADC_CHANNEL_INIT;
static AdcChannel s_adcTds = ADC_CHANNEL_INIT;
static AdcChannel s_adcPh = ADC_CHANNEL_INIT;
static PulseCapture s_capture = PULSE_CAPTURE_INIT;

static Bme280Config s_bmeConfig = {&s_i2c, BOARD_BME280_I2C_ADDRESS};
static Sgp30Config s_sgpConfig = {&s_i2c, BOARD_SGP30_I2C_ADDRESS};
static Ds18b20Config s_dsConfig = {&s_oneWire};
// Calibrations come from the applied configuration: copied in
// acquisitionInit(), then pushed to the drivers whenever a new revision is
// applied (on-device calibration, applyConfiguration()).
static LightConfig s_lightConfig = {&s_adcLight, {1.0f, 0.0f, 0, false}};
static TdsConfig s_tdsConfig = {&s_adcTds, {1.0f, 0.0f, 0, false}};
static PhConfig s_phConfig = {&s_adcPh, BOARD_PH_DIVIDER_TOP_OHM, BOARD_PH_DIVIDER_BOTTOM_OHM,
                              {0.0f, 0.0f, 0, false},
                              true};  // compiled nominal fit: published UNCALIBRATED
static Hcsr04Config s_hcConfig = {&s_capture, CFG_HCSR04_TIMEOUT_MS};

static Bme280Driver s_bme;
static Sgp30Driver s_sgp;
static Ds18b20Driver s_ds;
static LightDriver s_light;
static TdsDriver s_tds;
static PhDriver s_ph;
static Hcsr04Driver s_hc;

// ---- Per-driver stepping (init -> start -> poll), as test/bringup/water_main.c
enum { RUN_BME280 = 0, RUN_DS18B20, RUN_SGP30, RUN_HCSR04, RUN_TDS, RUN_PH, RUN_LIGHT, RUN_COUNT };

typedef struct DriverRun {
  SensorDriver* driver;
  uint32_t initBudgetMs;   // worst-case blocking of init()
  uint32_t stepBudgetMs;   // worst-case blocking of one start()/poll(); > 2 = "blocking"
  bool initialised;
  bool pending;            // started, not yet READY or failed
  bool wantStart;          // due this frame, not started yet
  MonoMs nextPollMs;
  MonoMs retryAtMs;        // earliest init() retry after a failure
  DriverStatus lastFailure;  // DRIVER_OK = none; logged on change only
  bool disabled;           // off by the configuration (WP8) or the lease (WP9): never scheduled
  uint32_t startEpoch;     // 5 V sensors: rail epoch when the measurement started
} DriverRun;

// Worst-case blocking from each driver header (§16 budgets).
static DriverRun s_runs[RUN_COUNT] = {
    [RUN_BME280] = {&s_bme.base, 30, 1, false, false, false, 0, 0, DRIVER_OK},
    [RUN_DS18B20] = {&s_ds.base, 50, 13, false, false, false, 0, 0, DRIVER_OK},
    [RUN_SGP30] = {&s_sgp.base, 45, 1, false, false, false, 0, 0, DRIVER_OK},
    [RUN_HCSR04] = {&s_hc.base, 1, 1, false, false, false, 0, 0, DRIVER_OK},
    [RUN_TDS] = {&s_tds.base, 1, 1, false, false, false, 0, 0, DRIVER_OK},
    [RUN_PH] = {&s_ph.base, 1, 1, false, false, false, 0, 0, DRIVER_OK},
    [RUN_LIGHT] = {&s_light.base, 1, 1, false, false, false, 0, 0, DRIVER_OK},
};

// One-second jobs, started in this order at each frame (§9): the 1-Wire work
// comes before the HC-SR04 ping because 1-Wire slots mask interrupts and
// would delay the echo timestamps (onewire_bus.h).
static const uint8_t kFrameOrder[] = {RUN_DS18B20, RUN_SGP30, RUN_HCSR04, RUN_TDS, RUN_PH, RUN_LIGHT};
#define FRAME_ORDER_COUNT ((uint8_t)(sizeof(kFrameOrder) / sizeof(kFrameOrder[0])))

enum { JOB_FRAME = 0, JOB_BME280, JOB_COUNT };
static ScheduleJob s_jobs[JOB_COUNT];

// ---- Applied configuration (WP8): read at frame boundaries only ---------------
static uint16_t s_appliedRevision;  // stamped into every Reading as configRevision
static ConfigBlob s_cfg;            // task-private copy (static: not on the stack)
#define BASELINE_OFFER_PERIOD_MS ((MonoMs)60 * 60 * 1000)  // Configuration rate-limits the write too
static MonoMs s_nextBaselineOfferMs;
static volatile Sgp30BaselineStatus s_sgpBaseline = SGP30_BASELINE_NONE;  // one writer, word-sized

// ---- Working snapshot and rule state (task-private) --------------------------
static Snapshot s_work;
static AlarmRuleState s_ruleState[POLICY_RULE_COUNT];

static portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
static AcquisitionStats s_stats;

static StaticTask_t s_tcb;
static StackType_t s_stack[4 * 1024];  // cfgTaskSpec(TASK_ID_ACQUISITION_NORMAL)->stackBytes
static TaskHandle_t s_task = NULL;
static StaticTask_t s_tcbLow;
static StackType_t s_stackLow[4 * 1024];  // cfgTaskSpec(TASK_ID_ACQUISITION_LOW_POWER)->stackBytes
static TaskHandle_t s_taskLow = NULL;

// ---- Lease (WP9, §14) ------------------------------------------------------------
// Everything above is shared by the two workers but touched only by the lease
// holder; the lock below orders the hand-over between the cores.
static portMUX_TYPE s_leaseMux = portMUX_INITIALIZER_UNLOCKED;
static PowerWorker s_owner = POWER_WORKER_NONE;
static uint32_t s_leaseGen;
static SensorMask s_allowed = SENSOR_MASK_ALL;
static uint32_t s_quiesceGen;   // requested, for the current lease
static uint32_t s_releasedGen;
static volatile bool s_halsReady;
static bool s_forceApply;       // a new holder applies the configuration at once

#define STATS_ADD(field, n)        \
  do {                             \
    portENTER_CRITICAL(&s_statsMux); \
    s_stats.field += (n);          \
    portEXIT_CRITICAL(&s_statsMux);  \
  } while (0)

// ---- Bookkeeping after a result ----------------------------------------------

static void evaluateRules(const Reading* r, MonoMs now) {
  for (uint8_t i = 0; i < POLICY_RULE_COUNT; ++i) {
    const AlarmRule* rule = &kPolicyRules[i];
    if (!rule->enabled || rule->metric != r->metric) continue;
    const RuleDecision d = evaluateRule(rule, r, now, alarmStaleLimitMs(r->metric), &s_ruleState[i]);
    if (d != RULE_DECISION_TRIP && d != RULE_DECISION_CLEAR) continue;
    AlarmEvent e;
    e.ruleId = i;
    e.decision = d;
    e.sample = *r;
#ifdef FW_ALARM_LATENCY_TEST
    safetyLatencyMarkSubmit();
    s_ruleState[i].tripped = false;  // re-arm: the next sample trips again
#endif
    safetySubmitEvent(&e);  // zero wait; wakes Safety now (§4)
    STATS_ADD(ruleEvents, 1);
  }
}

// Sensors on the external, switched 5 V rail (§14): gated on the rail epoch.
static bool onFiveVoltRail(const DriverRun* run) {
  return sensorMaskHas(SENSOR_MASK_RAIL_5V, sensorDriverId(run->driver));
}

// Every metric of the run's sensor shows `health` now. No new value (telemetry
// sends null), but a new observation: a new sequence, so the app records the
// transition and every repeated failure (contract: one row per seq).
static void markSensor(const DriverRun* run, Health health, MonoMs now) {
  const SensorId s = sensorDriverId(run->driver);
  for (uint8_t m = 0; m < METRIC_COUNT; ++m) {
    if (sensorOfMetric((MetricId)m) != s) continue;
    Reading* r = &s_work.readings[m];
    r->health = health;
    r->intervalStartMs = now;
    r->intervalEndMs = now;
    r->powerEpoch = railStateRead().epoch;
    ++r->sequence;
  }
}

// Stamp, store, check rules, for every reading the driver returned.
static void storeResults(DriverRun* run, MonoMs now, bool guardTimeout) {
  Reading out[4];
  const uint8_t n = sensorDriverReadResult(run->driver, out, 4);
  const RailState rail = railStateRead();
  const uint32_t epoch = rail.epoch;
  // The rail went down (or down and up) during the measurement: the number
  // may come from a collapsing supply, so it is never published (§14).
  const bool railLost = onFiveVoltRail(run) && (!rail.valid5v || rail.epoch != run->startEpoch);
  for (uint8_t k = 0; k < n; ++k) {
    Reading* r = &out[k];
    if (r->metric >= METRIC_COUNT) continue;
    if (guardTimeout) {  // abandoned at its deadline: a new, failed result
      r->health = HEALTH_TIMEOUT;
      r->qualityFlags = (uint16_t)(r->qualityFlags | kQualityLate);
      r->intervalEndMs = now;
    }
    if (railLost) r->health = HEALTH_UNPOWERED;
    // Every stored result is a new observation, failures included: the
    // drivers count only usable samples, which would give a FAULT after an OK
    // the OK's sequence (and the app would drop it as a re-send).
    r->sequence = s_work.readings[r->metric].sequence + 1u;
    r->powerEpoch = epoch;
    r->configRevision = s_appliedRevision;
    s_work.readings[r->metric] = *r;
    evaluateRules(r, now);
  }
  s_work.publishedMs = now;
  s_work.powerEpoch = epoch;
  snapshotPublish(&s_work);
}

static void noteFailure(DriverRun* run, DriverStatus st, const char* what) {
  if (st == run->lastFailure) return;  // log transitions only
  run->lastFailure = st;
  logPrintf(LOG_LEVEL_WARN, "acq: %s %s -> %s", sensorDriverName(run->driver), what,
            driverStatusName(st));
}

static void noteRecovered(DriverRun* run) {
  if (run->lastFailure == DRIVER_OK) return;
  run->lastFailure = DRIVER_OK;
  logPrintf(LOG_LEVEL_INFO, "acq: %s OK", sensorDriverName(run->driver));
}

// The HC-SR04 ping holds the flash gate from start to result (flash_gate.h).
static void releasePingGate(const DriverRun* run) {
  if (run == &s_runs[RUN_HCSR04]) flashGateGiveAfterPing();
}

static void finishRun(DriverRun* run, MonoMs now, DriverStatus st) {
  run->pending = false;
  releasePingGate(run);
  if (st == DRIVER_READY) {
    noteRecovered(run);
  } else {
    noteFailure(run, st, "result");
    STATS_ADD(driverFailures, 1);
    if (st == DRIVER_NOT_PRESENT || st == DRIVER_NOT_IMPLEMENTED) {
      run->initialised = false;
      run->retryAtMs = now;  // re-init at its next due time
    }
  }
  storeResults(run, now, false);
}

static void guardTimeout(DriverRun* run, MonoMs now) {
  sensorDriverCancel(run->driver);
  run->pending = false;
  releasePingGate(run);
  noteFailure(run, DRIVER_TIMEOUT, "guard");
  STATS_ADD(guardTimeouts, 1);
  storeResults(run, now, true);
}

// ---- Blocking rules (§9) -------------------------------------------------------

static bool isBlocking(uint32_t budgetMs) { return budgetMs > 2; }

// May a step blocking the worker for budgetMs run now? Not while a BME280
// conversion is pending (its poll would come too late and time out), not
// while an HC-SR04 echo is in flight (1-Wire masks interrupts), and only if
// it ends before the next BME280 slot.
static bool blockingAllowed(uint8_t runId, uint32_t budgetMs, MonoMs now) {
  if (!isBlocking(budgetMs)) return true;
  if (runId != RUN_BME280 && s_runs[RUN_BME280].pending) return false;
  if (s_runs[RUN_HCSR04].pending) return false;
  if (runId == RUN_BME280) return true;
  return scheduleFitsBefore(now, budgetMs, s_jobs[JOB_BME280].nextDueMs);
}

// The ping must not overlap a DS18B20 call: defer it if one is due soon.
static bool pingAllowed(MonoMs now) {
  const DriverRun* ds = &s_runs[RUN_DS18B20];
  if (ds->wantStart) return false;
  return !ds->pending || ds->nextPollMs > now + CFG_HCSR04_TIMEOUT_MS + 2;
}

// ---- Start and poll ------------------------------------------------------------

static void prepareInputs(uint8_t runId, MonoMs now) {
  if (runId == RUN_SGP30) {
    sgp30DriverSetHumidity(&s_sgp, &s_work.readings[METRIC_AIR_TEMPERATURE],
                           &s_work.readings[METRIC_AIR_HUMIDITY], now);
  } else if (runId == RUN_TDS) {
    tdsDriverSetWaterTemperature(&s_tds, &s_work.readings[METRIC_WATER_TEMPERATURE]);
  }
}

// Returns false if the start must wait (blocking rule); the job stays wanted.
static bool tryStart(uint8_t runId, MonoMs now) {
  DriverRun* run = &s_runs[runId];
  if (runId == RUN_HCSR04 && !pingAllowed(now)) return false;
  if (onFiveVoltRail(run)) {
    const RailState rail = railStateRead();
    if (!rail.valid5v) {  // no 5 V: no init, no conversion of a dead output
      markSensor(run, HEALTH_UNPOWERED, now);
      run->wantStart = false;
      return true;
    }
    run->startEpoch = rail.epoch;
  }
  if (!run->initialised) {
    if (!monoReached(now, run->retryAtMs)) {  // back-off: skip this period
      run->wantStart = false;
      return true;
    }
    if (!blockingAllowed(runId, run->initBudgetMs, now)) return false;
    const DriverStatus st = sensorDriverInit(run->driver);
    if (st != DRIVER_OK) {
      noteFailure(run, st, "init");
      run->retryAtMs = now + POLICY_INIT_RETRY_MS;
      run->wantStart = false;
      return true;
    }
    run->initialised = true;
    logPrintf(LOG_LEVEL_INFO, "acq: %s init OK", sensorDriverName(run->driver));
    now = monoNowMs();  // init may have taken tens of ms
  }
  if (!blockingAllowed(runId, run->stepBudgetMs, now)) return false;
  // No NVS write may overlap the echo window: defer the ping while one runs.
  if (runId == RUN_HCSR04 && !flashGateTryTakeForPing()) return false;
  prepareInputs(runId, now);
  run->wantStart = false;
  const DriverStatus st = sensorDriverStart(run->driver, now);
  if (st == DRIVER_OK) {
    run->pending = true;
    run->nextPollMs = now;
  } else {
    if (st != DRIVER_PENDING) run->initialised = false;
    finishRun(run, now, st);
  }
  return true;
}

// Returns true if the run made progress.
static bool tryPoll(uint8_t runId, MonoMs now) {
  DriverRun* run = &s_runs[runId];
  if (!run->pending || now < run->nextPollMs) return false;
  if (!blockingAllowed(runId, run->stepBudgetMs, now)) return false;
  const PollResult pr = sensorDriverPoll(run->driver, now);
  if (pr.status == DRIVER_PENDING) {
    run->nextPollMs = pr.nextPollMs > now ? pr.nextPollMs : now + 1;
    return true;
  }
  finishRun(run, monoNowMs(), pr.status);
  return true;
}

// ---- Frame and slot boundaries ---------------------------------------------------

// pH is provisional (UNCALIBRATED in pH units) until the user fits it on the
// device; the compiled nominal fit is never buffer-verified (D13).
static bool phProvisional(const ConfigBlob* cfg) { return (cfg->calOverridden & CAL_BIT_PH) == 0; }

// Drivers keep a private copy of their config: hand them the applied fits.
// A window in progress finishes with the old fit (its x is already taken).
static void applyCalibrations(const ConfigBlob* cfg) {
  phDriverSetCalibration(&s_ph, &cfg->ph, phProvisional(cfg));
  tdsDriverSetCalibration(&s_tds, &cfg->tds);
  lightDriverSetCalibration(&s_light, &cfg->light);
}

// Sensor on/off and calibration changes take effect here, between frames, by
// this task only: no other task ever touches a driver (§17).
static SensorMask allowedSensors(void) {
  portENTER_CRITICAL(&s_leaseMux);
  const SensorMask allowed = s_allowed;
  portEXIT_CRITICAL(&s_leaseMux);
  return allowed;
}

static void applyConfiguration(MonoMs now) {
  configurationPeekApplied(&s_cfg);
  if (s_cfg.revision == s_appliedRevision && !s_forceApply) return;
  s_forceApply = false;
  applyCalibrations(&s_cfg);
  const SensorMask allowed = allowedSensors();
  for (uint8_t i = 0; i < RUN_COUNT; ++i) {
    DriverRun* run = &s_runs[i];
    const SensorId id = sensorDriverId(run->driver);
    const bool wanted = sensorMaskHas(s_cfg.sensorEnabled, id);
    const bool enabled = wanted && sensorMaskHas(allowed, id);
    if (wanted && !enabled) markSensor(run, HEALTH_UNPOWERED, now);  // low power: its rail is off
    if (!enabled && !run->disabled) {
      if (run->pending) {
        sensorDriverCancel(run->driver);  // no preparePowerOff: the shared I2C bus stays in use
        run->pending = false;
        releasePingGate(run);
      }
      run->disabled = true;
      run->wantStart = false;
      logPrintf(LOG_LEVEL_INFO, "acq: %s disabled", sensorDriverName(run->driver));
    } else if (enabled && run->disabled) {
      run->disabled = false;
      run->initialised = false;  // init again at its next due time
      run->retryAtMs = now;
      run->lastFailure = DRIVER_OK;
      markSensor(run, HEALTH_WARMING, now);  // until its first result (§13 restore)
      logPrintf(LOG_LEVEL_INFO, "acq: %s enabled", sensorDriverName(run->driver));
    }
  }
  s_appliedRevision = s_cfg.revision;
  configurationAckApplied(s_appliedRevision);
}

static void offerBaseline(MonoMs now) {
  if (now < s_nextBaselineOfferMs) return;
  s_nextBaselineOfferMs = now + BASELINE_OFFER_PERIOD_MS;
  uint16_t co2eq, tvoc;
  if (!s_runs[RUN_SGP30].disabled && sgp30DriverGetBaseline(&s_sgp, &co2eq, &tvoc)) {
    configurationOfferBaseline(co2eq, tvoc);
  }
}

static void startFrame(MonoMs now) {
  applyConfiguration(now);
  offerBaseline(now);
  s_sgpBaseline = sgp30DriverBaselineStatus(&s_sgp);
  ScheduleJob* job = &s_jobs[JOB_FRAME];
  const uint32_t lateBefore = job->late;
  const uint32_t skippedBefore = job->skipped;
  scheduleJobMarkRun(job, now);

  // Frame guard: a one-second job still running from the last frame is
  // abandoned, never retried inside this frame (§18).
  for (uint8_t i = 0; i < FRAME_ORDER_COUNT; ++i) {
    DriverRun* run = &s_runs[kFrameOrder[i]];
    if (run->pending) guardTimeout(run, now);
    run->wantStart = !run->disabled;
  }

  ++s_work.frameSequence;
  s_work.publishedMs = now;
  snapshotPublish(&s_work);

  portENTER_CRITICAL(&s_statsMux);
  ++s_stats.frames;
  s_stats.frameLate += job->late - lateBefore;
  s_stats.frameSkipped += job->skipped - skippedBefore;
  s_stats.frameJitterMaxMs = job->maxLateMs;
  portEXIT_CRITICAL(&s_statsMux);
}

static void startBmeSlot(MonoMs now) {
  ScheduleJob* job = &s_jobs[JOB_BME280];
  const uint32_t lateBefore = job->late;
  const uint32_t skippedBefore = job->skipped;
  scheduleJobMarkRun(job, now);
  if (s_runs[RUN_BME280].pending) guardTimeout(&s_runs[RUN_BME280], now);
  // A disabled BME280 keeps its slot grid: blocking steps still respect it
  // (scheduleFitsBefore), which is conservative but never wrong.
  s_runs[RUN_BME280].wantStart = !s_runs[RUN_BME280].disabled;
  portENTER_CRITICAL(&s_statsMux);
  s_stats.bmeLate += job->late - lateBefore;
  s_stats.bmeSkipped += job->skipped - skippedBefore;
  portEXIT_CRITICAL(&s_statsMux);
}

// One pass: boundaries first, the BME280 fast lane next, then every due poll
// and the frame starts in order. Repeats while something progressed (each
// step is bounded, so the pass is too).
static void runPass(MonoMs now) {
  if (scheduleJobDue(&s_jobs[JOB_FRAME], now)) startFrame(now);
  if (scheduleJobDue(&s_jobs[JOB_BME280], now)) startBmeSlot(now);

  for (uint8_t iter = 0; iter < 8; ++iter) {
    bool progress = false;
    now = monoNowMs();
    if (s_runs[RUN_BME280].wantStart) progress |= tryStart(RUN_BME280, now);
    for (uint8_t r = 0; r < RUN_COUNT; ++r) progress |= tryPoll(r, monoNowMs());
    for (uint8_t i = 0; i < FRAME_ORDER_COUNT; ++i) {
      const uint8_t r = kFrameOrder[i];
      if (!s_runs[r].wantStart) continue;
      if (!tryStart(r, monoNowMs())) break;  // keep the order: later ones wait too
      progress = true;
    }
    if (!progress) break;
  }
}

// Next wake: the earliest job deadline or poll time that is not blocked
// (a blocked step is woken by whatever blocks it, which is in this set).
static MonoMs nextWake(MonoMs now) {
  MonoMs due = scheduleEarliestDue(s_jobs, JOB_COUNT);
  for (uint8_t r = 0; r < RUN_COUNT; ++r) {
    const DriverRun* run = &s_runs[r];
    if (!run->pending) continue;
    const MonoMs t = run->nextPollMs > now ? run->nextPollMs : now + 1;
    if (blockingAllowed(r, run->stepBudgetMs, t) && t < due) due = t;
  }
  return due;
}

// HALs begin in AcqNormal, on core 1: the echo ISR (and with it the GPIO ISR
// service the power-sense ISR shares) is installed on this core.
static void beginHals(void) {
  if (!i2cBusBegin(&s_i2c, BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_I2C_CLOCK_HZ)) {
    logPrintf(LOG_LEVEL_ERROR, "acq: I2C begin failed");
  }
  if (!oneWireBusBegin(&s_oneWire, BOARD_PIN_ONE_WIRE)) logPrintf(LOG_LEVEL_ERROR, "acq: 1-Wire begin failed");
  if (!adcChannelBegin(&s_adcLight, BOARD_PIN_LIGHT_ADC)) logPrintf(LOG_LEVEL_ERROR, "acq: light ADC failed");
  if (!adcChannelBegin(&s_adcTds, BOARD_PIN_TDS_ADC)) logPrintf(LOG_LEVEL_ERROR, "acq: TDS ADC failed");
  if (!adcChannelBegin(&s_adcPh, BOARD_PIN_PH_ADC)) logPrintf(LOG_LEVEL_ERROR, "acq: pH ADC failed");
  if (!pulseCaptureBegin(&s_capture, BOARD_PIN_HCSR04_TRIG, BOARD_PIN_HCSR04_ECHO)) {
    logPrintf(LOG_LEVEL_ERROR, "acq: HC-SR04 capture failed");
  }
}

static TaskHandle_t workerTask(PowerWorker w) { return w == POWER_WORKER_LOW ? s_taskLow : s_task; }

static bool holdsLease(PowerWorker me) {
  portENTER_CRITICAL(&s_leaseMux);
  const bool mine = s_owner == me;
  portEXIT_CRITICAL(&s_leaseMux);
  return mine;
}

static bool quiesceRequested(void) {
  portENTER_CRITICAL(&s_leaseMux);
  const bool requested = s_quiesceGen != 0 && s_quiesceGen == s_leaseGen;
  portEXIT_CRITICAL(&s_leaseMux);
  return requested;
}

// New holder: join the task watchdog, restart the schedule from now (the
// previous holder's deadlines are not ours to count as late) and apply the
// configuration within the allowed sensors at the first frame.
static void acquireLease(PowerWorker me, MonoMs now) {
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "acq: task watchdog add failed");
  scheduleJobInit(&s_jobs[JOB_FRAME], CFG_FRAME_PERIOD_MS, CFG_FRAME_RESERVATION_MS, now);
  scheduleJobInit(&s_jobs[JOB_BME280], CFG_BME280_PERIOD_MS, CFG_BME280_BUDGET_MS, now);
  for (uint8_t i = 0; i < RUN_COUNT; ++i) {
    s_runs[i].pending = false;
    s_runs[i].wantStart = false;
  }
  s_forceApply = true;
  progressMark(me == POWER_WORKER_NORMAL ? TASK_ID_ACQUISITION_NORMAL : TASK_ID_ACQUISITION_LOW_POWER);
  logPrintf(LOG_LEVEL_INFO, "acq: %s worker holds the lease (gen %lu, sensors 0x%02x)",
            me == POWER_WORKER_NORMAL ? "normal" : "low-power", (unsigned long)s_leaseGen,
            (unsigned)allowedSensors());
}

// QUIESCE -> RELEASED (§14): nothing is left running, the 5 V interfaces are
// safe if their rail is down, and the worker never touches a driver again
// until the next grant.
static void releaseLease(PowerWorker me) {
  const bool railDown = !railStateRead().valid5v;
  for (uint8_t i = 0; i < RUN_COUNT; ++i) {
    DriverRun* run = &s_runs[i];
    if (run->pending) {
      sensorDriverCancel(run->driver);
      run->pending = false;
      releasePingGate(run);
    }
    run->wantStart = false;
    if (railDown && onFiveVoltRail(run)) {
      sensorDriverPreparePowerOff(run->driver);  // HC-SR04: TRIG held low, no drive into a dead module
      run->initialised = false;
    }
  }
  snapshotPublish(&s_work);
  esp_task_wdt_delete(NULL);  // a blocked worker owes the watchdog nothing
  portENTER_CRITICAL(&s_leaseMux);
  s_owner = POWER_WORKER_NONE;
  s_releasedGen = s_leaseGen;
  s_quiesceGen = 0;
  portEXIT_CRITICAL(&s_leaseMux);
  logPrintf(LOG_LEVEL_INFO, "acq: %s worker released (gen %lu)", me == POWER_WORKER_NORMAL ? "normal" : "low-power",
            (unsigned long)s_releasedGen);
}

static void acqTask(void* arg) {
  const PowerWorker me = (PowerWorker)(uintptr_t)arg;
  const TaskId id = me == POWER_WORKER_NORMAL ? TASK_ID_ACQUISITION_NORMAL : TASK_ID_ACQUISITION_LOW_POWER;
  if (me == POWER_WORKER_NORMAL) {
    beginHals();
    s_halsReady = true;
    portENTER_CRITICAL(&s_leaseMux);
    const PowerWorker owner = s_owner;
    portEXIT_CRITICAL(&s_leaseMux);
    if (owner == POWER_WORKER_LOW && s_taskLow != NULL) xTaskNotifyGive(s_taskLow);  // granted before the HALs
  }
  const MonoMs t0 = monoNowMs();
  (void)t0;
  bool holding = false;

  // Task watchdog (§18): the holder resets it after each pass. A pass happens
  // at least every 250 ms (BME280 slot), so 5 s of silence means the task is
  // stuck inside a call. A worker without the lease is not on the watchdog.
  for (;;) {
    if (!holding) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // a grant (or a stale wake-up)
      if (!s_halsReady || !holdsLease(me)) continue;
      holding = true;
      acquireLease(me, monoNowMs());
    }
    if (quiesceRequested()) {
      releaseLease(me);
      holding = false;
      continue;
    }
    const MonoMs now = monoNowMs();
    const MonoMs due = nextWake(now);
    ulTaskNotifyTake(pdTRUE, due > now ? pdMS_TO_TICKS((uint32_t)(due - now)) : 0);
    if (quiesceRequested()) continue;  // released at the top, before any new step
    runPass(monoNowMs());
#ifdef FW_INJECT_STALL_ACQ
    // Acceptance test (§19 "Stall injection"): block forever after N ms.
    if (monoNowMs() - t0 >= (MonoMs)(FW_INJECT_STALL_ACQ)) {
      logPrintf(LOG_LEVEL_WARN, "acq: INJECTED STALL - blocking forever");
      vTaskDelay(portMAX_DELAY);
    }
#endif
    progressMark(id);
    esp_task_wdt_reset();
  }
}

TaskHandle_t acquisitionTaskHandle(void) {
  portENTER_CRITICAL(&s_leaseMux);
  const PowerWorker owner = s_owner;
  portEXIT_CRITICAL(&s_leaseMux);
  return owner == POWER_WORKER_LOW ? s_taskLow : s_task;
}

TaskHandle_t acquisitionNormalTaskHandle(void) { return s_task; }
TaskHandle_t acquisitionLowPowerTaskHandle(void) { return s_taskLow; }

static TaskHandle_t createWorker(TaskId id, PowerWorker w, StackType_t* stack, uint32_t stackBytes,
                                 StaticTask_t* tcb) {
  const TaskSpec* spec = cfgTaskSpec(id);
  if (spec->stackBytes != stackBytes) {
    logPrintf(LOG_LEVEL_WARN, "acq: %s stack %lu B, task table says %lu B", spec->name, (unsigned long)stackBytes,
              (unsigned long)spec->stackBytes);
  }
  TaskHandle_t t = xTaskCreateStaticPinnedToCore(acqTask, spec->name, stackBytes, (void*)(uintptr_t)w,
                                                 spec->priority, stack, tcb, spec->core);
  if (t != NULL) {
    logPrintf(LOG_LEVEL_INFO, "acq: %s started (core %u, prio %u)", spec->name, (unsigned)spec->core,
              (unsigned)spec->priority);
  }
  return t;
}

bool acquisitionInit(void) {
  if (s_task != NULL) return true;
  // Applied configuration: calibrations now, the sensor mask at the first frame.
  configurationPeekApplied(&s_cfg);
  s_phConfig.calibration = s_cfg.ph;
  s_phConfig.calibrationProvisional = phProvisional(&s_cfg);
  s_tdsConfig.calibration = s_cfg.tds;
  s_lightConfig.calibration = s_cfg.light;
  bme280DriverConstruct(&s_bme, &s_bmeConfig);
  sgp30DriverConstruct(&s_sgp, &s_sgpConfig);
  ds18b20DriverConstruct(&s_ds, &s_dsConfig);
  lightDriverConstruct(&s_light, &s_lightConfig);
  tdsDriverConstruct(&s_tds, &s_tdsConfig);
  phDriverConstruct(&s_ph, &s_phConfig);
  hcsr04DriverConstruct(&s_hc, &s_hcConfig);
  uint16_t co2eq, tvoc;
  if (configurationStoredBaseline(&co2eq, &tvoc)) {
    sgp30DriverSetBaseline(&s_sgp, co2eq, tvoc);  // restored at the SGP30's init
    logPrintf(LOG_LEVEL_INFO, "acq: SGP30 baseline restored from NVS");
  }
  s_appliedRevision = 0;  // revisions start at 1: forces applyConfiguration() at the first frame
  s_nextBaselineOfferMs = BASELINE_OFFER_PERIOD_MS;
  snapshotReset(&s_work);
  memset(s_ruleState, 0, sizeof(s_ruleState));

  // No lease yet: PowerCoordinator grants the first one (§14).
  s_task = createWorker(TASK_ID_ACQUISITION_NORMAL, POWER_WORKER_NORMAL, s_stack, sizeof(s_stack), &s_tcb);
  s_taskLow = createWorker(TASK_ID_ACQUISITION_LOW_POWER, POWER_WORKER_LOW, s_stackLow, sizeof(s_stackLow),
                           &s_tcbLow);
  if (s_taskLow == NULL) logPrintf(LOG_LEVEL_ERROR, "acq: low-power worker missing - no sensing in low power");
  return s_task != NULL;
}

bool acquisitionGrant(PowerWorker worker, uint32_t generation, SensorMask allowed) {
  if (worker != POWER_WORKER_NORMAL && worker != POWER_WORKER_LOW) return false;
  portENTER_CRITICAL(&s_leaseMux);
  const bool free = s_owner == POWER_WORKER_NONE;
  if (free) {
    s_owner = worker;
    s_leaseGen = generation;
    s_allowed = (SensorMask)(allowed & SENSOR_MASK_ALL);
    s_quiesceGen = 0;
  }
  portEXIT_CRITICAL(&s_leaseMux);
  if (!free) {
    logPrintf(LOG_LEVEL_ERROR, "acq: grant gen %lu refused - the lease is still held", (unsigned long)generation);
    return false;
  }
  TaskHandle_t t = workerTask(worker);
  if (t != NULL) xTaskNotifyGive(t);
  return true;
}

bool acquisitionRequestQuiesce(uint32_t generation) {
  portENTER_CRITICAL(&s_leaseMux);
  const bool current = s_owner != POWER_WORKER_NONE && s_leaseGen == generation;
  const PowerWorker owner = s_owner;
  if (current) s_quiesceGen = generation;
  portEXIT_CRITICAL(&s_leaseMux);
  if (!current) return false;
  TaskHandle_t t = workerTask(owner);
  if (t != NULL) xTaskNotifyGive(t);  // wakes it from a bounded wait
  return true;
}

uint32_t acquisitionReleasedGeneration(void) {
  portENTER_CRITICAL(&s_leaseMux);
  const uint32_t gen = s_releasedGen;
  portEXIT_CRITICAL(&s_leaseMux);
  return gen;
}

bool acquisitionHalsReady(void) { return s_halsReady; }

TaskId acquisitionActiveTaskId(void) {
  portENTER_CRITICAL(&s_leaseMux);
  const PowerWorker owner = s_owner;
  portEXIT_CRITICAL(&s_leaseMux);
  if (owner == POWER_WORKER_NORMAL) return TASK_ID_ACQUISITION_NORMAL;
  if (owner == POWER_WORKER_LOW) return TASK_ID_ACQUISITION_LOW_POWER;
  return TASK_ID_COUNT;
}

const char* acquisitionSgp30BaselineName(void) {
  switch (s_sgpBaseline) {  // contract v2 spelling (lowercase)
    case SGP30_BASELINE_LEARNING: return "learning";
    case SGP30_BASELINE_RESTORED: return "restored";
    case SGP30_BASELINE_VALID: return "valid";
    default: return "none";
  }
}

void acquisitionGetStats(AcquisitionStats* out) {
  portENTER_CRITICAL(&s_statsMux);
  *out = s_stats;
  portEXIT_CRITICAL(&s_statsMux);
}

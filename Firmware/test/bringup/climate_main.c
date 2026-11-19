// Combined bring-up for the shared I2C bus: BME280 (WP1) + SGP30 (WP6).
//   pio run -e test_climate -t upload -t monitor
// Both on the retained 3.3 V supply, SDA 21 / SCL 47, addresses 0x76/0x77 and
// 0x58. Not the production scheduler (WP7): one task owns the bus and runs a
// 250 ms tick. Each tick runs one BME280 measurement; every 4th tick (1 s) it
// passes the fresh BME280 T/RH to the SGP30 as humidity compensation, then
// runs one SGP30 measurement. One labelled line per second:
//   t=   17s  BME280  T  25.62 C  RH  58.28 %  P 1022.96 hPa  AH 13.87 g/m3  |
//     SGP30  TVOC     5 ppb  CO2eq   413 ppm  LEARNING          (one line)
// A value that is not usable shows its health instead (FAULT, warm, ...).
// state = SGP30 health: WARMING, LEARNING (baseline not yet learned, D13),
// OK, DEP_INVALID (humidity input stale), FAULT; "nocomp" = no humidity
// compensation. Events (init, faults, baseline and compensation changes,
// a driver over its time budget) print as their own "# ..." lines.
// Replaces src/main.c in this env only; links the production drivers.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "domain/sgp30_codec.h"
#include "drivers/bme280.h"
#include "drivers/sgp30.h"
#include "hal/clock.h"
#include "hal/i2c_bus.h"

#define CLIMATE_SGP30_EVERY_TICKS 4  // 4 x 250 ms = the SGP30's 1 s cadence
#define CLIMATE_SGP30_BUDGET_MS 15   // architecture §9 host budget

static I2cBus s_i2c = I2C_BUS_INIT;
static Bme280Config s_bmeConfig;  // address chosen at runtime in setup()
static Bme280Driver s_bme;
static Sgp30Config s_sgpConfig = {&s_i2c, BOARD_SGP30_I2C_ADDRESS};
static Sgp30Driver s_sgp;

typedef struct DriverRun {
  SensorDriver* driver;
  bool initialised;
  uint32_t guardMs;  // harness-level guard, not the driver budget
  Reading out[4];
  uint8_t n;
  uint32_t elapsedMs;
} DriverRun;

// One init (if needed) -> start -> poll until READY/error -> readResult.
// Same logic as bringupTask(), for one measurement. true if readings arrived.
static bool runOnce(DriverRun* run) {
  SensorDriver* drv = run->driver;
  run->n = 0;
  if (!run->initialised) {
    const DriverStatus st = sensorDriverInit(drv);
    printf("# %s init -> %s\n", sensorDriverName(drv), driverStatusName(st));
    if (st != DRIVER_OK) return false;
    run->initialised = true;
  }

  const MonoMs t0 = monoNowMs();
  const DriverStatus st = sensorDriverStart(drv, t0);
  if (st != DRIVER_OK) {
    printf("# %s start -> %s\n", sensorDriverName(drv), driverStatusName(st));
    if (st != DRIVER_PENDING) run->initialised = false;
    return false;
  }

  PollResult pr = sensorDriverPoll(drv, monoNowMs());
  while (pr.status == DRIVER_PENDING) {
    const MonoMs now = monoNowMs();
    if (now - t0 >= run->guardMs) {
      sensorDriverCancel(drv);
      pr.status = DRIVER_TIMEOUT;
      break;
    }
    uint32_t waitMs = 1;
    if (pr.nextPollMs > now) waitMs = (uint32_t)(pr.nextPollMs - now);
    const TickType_t ticks = pdMS_TO_TICKS(waitMs);
    vTaskDelay(ticks > 0 ? ticks : 1);
    pr = sensorDriverPoll(drv, monoNowMs());
  }

  run->elapsedMs = (uint32_t)(monoNowMs() - t0);
  if (pr.status != DRIVER_READY) {
    printf("# %s poll -> %s after %lu ms\n", sensorDriverName(drv),
           driverStatusName(pr.status), (unsigned long)run->elapsedMs);
    if (pr.status == DRIVER_NOT_PRESENT) run->initialised = false;
    return false;
  }
  run->n = sensorDriverReadResult(drv, run->out, 4);
  return true;
}

static void printSgp30Baseline(void) {
  static Sgp30BaselineStatus lastStatus = SGP30_BASELINE_NONE;
  static uint16_t lastCo2eq = 0;
  static uint16_t lastTvoc = 0;
  const Sgp30BaselineStatus st = sgp30DriverBaselineStatus(&s_sgp);
  if (st != lastStatus) {
    printf("# sgp30 baseline %s (feature set 0x%04X)\n", sgp30BaselineStatusName(st),
           (unsigned)sgp30DriverFeatureSet(&s_sgp));
    lastStatus = st;
  }
  uint16_t co2eq = 0;
  uint16_t tvoc = 0;
  if (sgp30DriverGetBaseline(&s_sgp, &co2eq, &tvoc) && (co2eq != lastCo2eq || tvoc != lastTvoc)) {
    printf("# sgp30 baseline co2eq 0x%04X tvoc 0x%04X (store these)\n", (unsigned)co2eq,
           (unsigned)tvoc);
    lastCo2eq = co2eq;
    lastTvoc = tvoc;
  }
}

// First timing once, then only a run over its budget.
static void checkTiming(const char* name, const DriverRun* run, uint32_t budgetMs,
                        bool* reported) {
  if (!*reported) {
    printf("# %s took %lu ms (budget %lu ms)\n", name, (unsigned long)run->elapsedMs,
           (unsigned long)budgetMs);
    *reported = true;
  } else if (run->elapsedMs > budgetMs) {
    printf("# %s took %lu ms, over its %lu ms budget\n", name, (unsigned long)run->elapsedMs,
           (unsigned long)budgetMs);
  }
}

// One labelled value, e.g. "T 25.65 C". Not usable: its health, e.g. "T FAULT".
static void printCell(const char* label, const Reading* r, int width, int decimals,
                      float scale, const char* unit) {
  const Health h = r->health;
  if (h == HEALTH_OK || h == HEALTH_UNCALIBRATED || h == HEALTH_DEPENDENCY_INVALID) {
    printf("  %s %*.*f %s", label, width, decimals, (double)(r->value * scale), unit);
    return;
  }
  const char* why = h == HEALTH_WARMING ? "warm" : h == HEALTH_STARTING ? "-" : healthName(h);
  printf("  %s %*s %*s", label, width, why, (int)strlen(unit), "");
}

static const char* sgp30State(const Reading* r) {
  switch (r->health) {
    case HEALTH_OK: return "OK";
    case HEALTH_UNCALIBRATED: return "LEARNING";
    case HEALTH_DEPENDENCY_INVALID: return "DEP_INVALID";
    case HEALTH_STARTING: return "-";
    default: return healthName(r->health);
  }
}

// e.g.  t=64s  BME280  T 25.65 C  RH 57.70 %  P 1022.89 hPa  AH 13.74 g/m3  |
//       SGP30  TVOC   22 ppb  CO2eq  413 ppm  LEARNING      (one line)
static void printRow(MonoMs now, const Reading* bme, bool ahValid, float ah,
                     const Reading* sgp) {
  printf("t=%5lus  BME280", (unsigned long)(now / 1000));
  printCell("T", &bme[0], 6, 2, 1.0f, "C");
  printCell("RH", &bme[1], 6, 2, 1.0f, "%");
  printCell("P", &bme[2], 7, 2, 0.01f, "hPa");  // Pa -> hPa
  if (ahValid) {
    printf("  AH %5.2f g/m3", (double)ah);
  } else {
    printf("  AH %5s     ", "-");
  }
  printf("  |  SGP30");
  printCell("TVOC", &sgp[0], 5, 0, 1.0f, "ppb");
  printCell("CO2eq", &sgp[1], 5, 0, 1.0f, "ppm");
  printf("  %s%s\n", sgp30State(&sgp[0]),
         (sgp[0].qualityFlags & kQualityUncompensated) ? " nocomp" : "");
}

static void climateTask(void* arg) {
  (void)arg;
  DriverRun bme = {&s_bme.base, false, CFG_BME280_PERIOD_MS, {{0}}, 0, 0};
  DriverRun sgp = {&s_sgp.base, false, CFG_BME280_PERIOD_MS, {{0}}, 0, 0};
  bool bmeTimed = false;
  bool sgpTimed = false;
  int lastCompensated = -1;  // unknown
  uint32_t tick = 0;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CFG_BME280_PERIOD_MS));
    const bool secondTick = (tick++ % CLIMATE_SGP30_EVERY_TICKS) == 0;

    if (runOnce(&bme)) checkTiming("bme280", &bme, CFG_BME280_BUDGET_MS, &bmeTimed);
    if (!secondTick) continue;

    // Readings 0/1 are air temperature and humidity (bme280.h). A failed
    // BME280 run leaves the last readings, which then age out as stale.
    const MonoMs now = monoNowMs();
    Reading latest[BME280_METRICS];
    sensorDriverReadResult(&s_bme.base, latest, BME280_METRICS);
    sgp30DriverSetHumidity(&s_sgp, &latest[0], &latest[1], now);
    const bool fresh = readingIsActionableAt(&latest[0], now, CFG_STALE_BME280_MS) &&
                       readingIsActionableAt(&latest[1], now, CFG_STALE_BME280_MS);
    const float ah = fresh ? sgp30AbsoluteHumidity(latest[0].value, latest[1].value) : 0.0f;
    if ((int)fresh != lastCompensated) {
      printf(fresh ? "# sgp30 humidity compensation ON (from BME280)\n"
                   : "# BME280 T/RH not fresh -> sgp30 humidity compensation OFF\n");
      lastCompensated = fresh;
    }

    if (runOnce(&sgp)) checkTiming("sgp30", &sgp, CLIMATE_SGP30_BUDGET_MS, &sgpTimed);
    Reading sgpOut[SGP30_METRICS];
    sensorDriverReadResult(&s_sgp.base, sgpOut, SGP30_METRICS);
    printRow(now, latest, fresh, ah, sgpOut);
    printSgp30Baseline();
  }
}

void setup(void) {
  delay(200);
  printf("\n=== bring-up: climate (bme280 + sgp30, SDA %d, SCL %d, %lu Hz) ===\n",
         BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, (unsigned long)BOARD_I2C_CLOCK_HZ);

  const bool busOk = i2cBusBegin(&s_i2c, BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL,
                                 BOARD_I2C_CLOCK_HZ);
  printf("i2c begin -> %s\n", busOk ? "OK" : "FAILED");

  // BME280: whichever address ACKs (SDO strap); default if neither does.
  uint8_t bmeAddress = BOARD_BME280_I2C_ADDRESS;
  const bool ack76 = i2cBusProbe(&s_i2c, BOARD_BME280_I2C_ADDRESS);
  const bool ack77 = i2cBusProbe(&s_i2c, BOARD_BME280_I2C_ADDRESS_ALT);
  const bool ack58 = i2cBusProbe(&s_i2c, BOARD_SGP30_I2C_ADDRESS);
  printf("probe bme280 0x%02X -> %s, 0x%02X -> %s | sgp30 0x%02X -> %s\n",
         BOARD_BME280_I2C_ADDRESS, ack76 ? "ACK" : "-", BOARD_BME280_I2C_ADDRESS_ALT,
         ack77 ? "ACK" : "-", BOARD_SGP30_I2C_ADDRESS, ack58 ? "ACK" : "-");
  if (!ack76 && ack77) bmeAddress = BOARD_BME280_I2C_ADDRESS_ALT;

  s_bmeConfig.bus = &s_i2c;
  s_bmeConfig.address = bmeAddress;
  bme280DriverConstruct(&s_bme, &s_bmeConfig);
  sgp30DriverConstruct(&s_sgp, &s_sgpConfig);

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_ACQUISITION_NORMAL);
  xTaskCreatePinnedToCore(climateTask, "climate", spec->stackBytes, NULL, spec->priority, NULL,
                          spec->core);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

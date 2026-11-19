// Combined bring-up for the water probes: DS18B20 (WP2) + SEN0244 TDS (WP5).
//   pio run -e test_water -t upload -t monitor
// DS18B20 on GPIO14 (1-Wire, retained 3.3 V); TDS A on GPIO6 (ADC1_CH5,
// switched 5 V rail must be ON). Not the production scheduler (WP7): one task
// owns both buses and runs a 1 s frame. Each frame it passes the latest
// DS18B20 reading to the TDS driver (tdsDriverSetWaterTemperature), then runs
// both measurements interleaved: the DS18B20 converts for 750 ms while the
// TDS window (~560 ms) is sampled, so both fit one frame. TDS is therefore
// compensated with the previous frame's water temperature (~1 s old, inside
// the 3 s staleness limit); the first frame has none (NO TEMP).
//
// One table row per frame (header repeated every WATER_HEADER_EVERY rows):
//         | DS18B20 |  SEN0244 TDS                                |
//    time | water T |    raw | uncomp |   @25C | state   | trend    |
//      12 |   24.81 |   1245 |    470 |    466 | UNCAL   | stable   |
//   water T  DS18B20 degC, or why not: FAULT, NOPRES (no presence pulse),
//            TIMEOUT (frame guard), "-" (no result yet)
//   raw      TDS pin mV (filtered window)
//   uncomp   ppm from raw with NO temperature compensation
//   @25C     ppm compensated to 25 degC (what production uses); "-" without
//            a fresh water temperature
//   state    OK, UNCAL (nominal curve, no K-factor, D13), NO TEMP (no fresh
//            water temperature), OVER (above 1000 ppm or ADC clipped), FAULT
//   trend    stable once the ppm shown (@25C, or uncomp when there is none)
//            has stopped drifting: over the last WATER_TREND_ROWS rows the mean
//            of the newer half is within 0.4 % (min 2 ppm) of the older half,
//            and no row is more than 2 % off. Otherwise settling. Judged on
//            @25C, comparing half-means, because the DS18B20 creeps towards
//            the water temperature for a minute or more (~0.15 %/s in ppm),
//            slower than the row-to-row noise.
// Events (init, faults, ROM code, compensation on/off, first timings) print
// as their own "# ..." lines between rows.
// Replaces src/main.c in this env only; links the production drivers.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "domain/tds_codec.h"
#include "drivers/ds18b20.h"
#include "drivers/tds.h"
#include "hal/adc_channel.h"
#include "hal/clock.h"
#include "hal/onewire_bus.h"

#define WATER_HEADER_EVERY 20     // rows between repeated table headers
#define WATER_TREND_ROWS 10           // rows in the window (even: two halves)
#define WATER_TREND_DRIFT_PCT 0.4f    // newer-half vs older-half mean, % ...
#define WATER_TREND_MIN_PPM 2.0f      // ... but never tighter than this
#define WATER_TREND_SPREAD_PCT 2.0f   // any single row vs the mean

static OneWireBus s_oneWire = ONE_WIRE_BUS_INIT;
static Ds18b20Config s_dsConfig = {&s_oneWire};
static Ds18b20Driver s_ds;
static AdcChannel s_adc = ADC_CHANNEL_INIT;
static TdsConfig s_tdsConfig = {&s_adc, {1.0f, 0.0f, 0, false}};  // nominal curve, no K-factor
static TdsDriver s_tds;

typedef struct DriverRun {
  SensorDriver* driver;
  bool initialised;
  bool pending;    // started, not yet READY or failed
  bool ready;      // this frame produced a result
  DriverStatus failure;  // why not ready (DRIVER_OK = not started this frame)
  MonoMs t0;
  MonoMs nextPollMs;
  uint32_t elapsedMs;
} DriverRun;

// Last rows, for the trend column.
typedef struct Trend {
  uint8_t count;  // valid rows, up to WATER_TREND_ROWS
  uint8_t next;
  bool compensated;  // which column the window holds (@25C or uncomp)
  float ppm[WATER_TREND_ROWS];
} Trend;

// init (if needed) -> start. Same logic as bringupTask(), without waiting.
static void beginRun(DriverRun* run, MonoMs now) {
  SensorDriver* drv = run->driver;
  run->pending = false;
  run->ready = false;
  run->failure = DRIVER_OK;
  if (!run->initialised) {
    const DriverStatus st = sensorDriverInit(drv);
    printf("# %s init -> %s\n", sensorDriverName(drv), driverStatusName(st));
    if (st != DRIVER_OK) {
      run->failure = st;
      return;
    }
    run->initialised = true;
  }
  run->t0 = now;
  const DriverStatus st = sensorDriverStart(drv, now);
  if (st != DRIVER_OK) {
    printf("# %s start -> %s\n", sensorDriverName(drv), driverStatusName(st));
    if (st != DRIVER_PENDING) run->initialised = false;
    run->failure = st;
    return;
  }
  run->pending = true;
  run->nextPollMs = now;
}

// One poll if it is due. Clears `pending` once READY or failed.
static void stepRun(DriverRun* run, MonoMs now) {
  if (!run->pending || now < run->nextPollMs) return;
  SensorDriver* drv = run->driver;
  const PollResult pr = sensorDriverPoll(drv, now);
  if (pr.status == DRIVER_PENDING) {
    run->nextPollMs = pr.nextPollMs > now ? pr.nextPollMs : now + 1;
    return;
  }
  run->pending = false;
  run->elapsedMs = (uint32_t)(monoNowMs() - run->t0);
  if (pr.status != DRIVER_READY) {
    printf("# %s poll -> %s after %lu ms\n", sensorDriverName(drv),
           driverStatusName(pr.status), (unsigned long)run->elapsedMs);
    if (pr.status == DRIVER_NOT_PRESENT) run->initialised = false;
    run->failure = pr.status;
    return;
  }
  run->ready = true;
}

// Harness-level guard, not a driver budget: abandon what is left of the frame.
static void cancelRun(DriverRun* run) {
  if (!run->pending) return;
  sensorDriverCancel(run->driver);
  run->pending = false;
  run->failure = DRIVER_TIMEOUT;
  printf("# %s TIMEOUT (frame guard)\n", sensorDriverName(run->driver));
}

static void printTimingOnce(const DriverRun* run, bool* reported) {
  if (*reported || !run->ready) return;
  printf("# %s took %lu ms\n", sensorDriverName(run->driver), (unsigned long)run->elapsedMs);
  *reported = true;
}

static void printRomIfNew(void) {
  static uint8_t last[8];
  static bool printed = false;
  uint8_t rom[8];
  if (!ds18b20DriverGetRom(&s_ds, rom)) return;
  if (printed && memcmp(rom, last, sizeof(rom)) == 0) return;
  memcpy(last, rom, sizeof(rom));
  printed = true;
  printf("# ds18b20 ROM %02X-%02X%02X%02X%02X%02X%02X crc %02X\n", rom[0], rom[6], rom[5],
         rom[4], rom[3], rom[2], rom[1], rom[7]);
}

static void printHeader(void) {
  printf("\n");
  printf("        | DS18B20 |  SEN0244 TDS                                |\n");
  printf("   time | water T |    raw | uncomp |   @25C | state   | trend    |\n");
  printf("      s |    degC |     mV |    ppm |    ppm |         |          |\n");
  printf("--------+---------+--------+--------+--------+---------+----------+\n");
}

// Water temperature cell: the value, or why there is none this frame.
static void waterCell(const DriverRun* run, const Reading* t, char* cell, size_t len) {
  if (!run->ready) {
    const char* why = run->failure == DRIVER_NOT_PRESENT ? "NOPRES"
                      : run->failure == DRIVER_TIMEOUT   ? "TIMEOUT"
                      : run->failure == DRIVER_OK        ? "-"
                                                         : "FAULT";
    snprintf(cell, len, "%s", why);
  } else if (t->health == HEALTH_OK) {
    snprintf(cell, len, "%.2f", (double)t->value);
  } else {
    snprintf(cell, len, "%s", t->health == HEALTH_STARTING ? "-" : healthName(t->health));
  }
}

static const char* tdsStateName(const Reading* r) {
  switch (r->health) {
    case HEALTH_OK: return "OK";
    case HEALTH_UNCALIBRATED: return "UNCAL";
    case HEALTH_DEPENDENCY_INVALID: return "NO TEMP";
    case HEALTH_STARTING: return "-";
    case HEALTH_FAULT: return (r->qualityFlags & kQualitySaturated) ? "OVER" : "FAULT";
    default: return healthName(r->health);
  }
}

// Add this row and return "stable" / "settling" / "-". `ppm` is the value
// shown: @25C when compensated, else uncomp. A gap or a switch between the
// two restarts the window.
static const char* trendUpdate(Trend* tr, bool tdsOk, bool compensated, float ppm) {
  if (!tdsOk) {
    tr->count = 0;
    return "-";
  }
  if (tr->count > 0 && compensated != tr->compensated) tr->count = 0;
  if (tr->count == 0) tr->next = 0;
  tr->compensated = compensated;
  tr->ppm[tr->next] = ppm;
  tr->next = (uint8_t)((tr->next + 1) % WATER_TREND_ROWS);
  if (tr->count < WATER_TREND_ROWS) ++tr->count;
  if (tr->count < WATER_TREND_ROWS) return "settling";

  // Oldest row is at `next` (the window is full).
  const uint8_t half = WATER_TREND_ROWS / 2;
  float older = 0.0f, newer = 0.0f;
  for (uint8_t i = 0; i < WATER_TREND_ROWS; ++i) {
    const float v = tr->ppm[(tr->next + i) % WATER_TREND_ROWS];
    if (i < half) older += v; else newer += v;
  }
  older /= half;
  newer /= half;
  const float mean = (older + newer) / 2.0f;
  float drift = mean * WATER_TREND_DRIFT_PCT / 100.0f;
  if (drift < WATER_TREND_MIN_PPM) drift = WATER_TREND_MIN_PPM;
  float spread = mean * WATER_TREND_SPREAD_PCT / 100.0f;
  if (spread < WATER_TREND_MIN_PPM) spread = WATER_TREND_MIN_PPM;
  if (newer - older > drift || older - newer > drift) return "settling";
  for (uint8_t i = 0; i < WATER_TREND_ROWS; ++i) {
    const float d = tr->ppm[i] - mean;
    if (d > spread || -d > spread) return "settling";
  }
  return "stable";
}

static void printRow(MonoMs now, const DriverRun* ds, const Reading* t, const DriverRun* tds,
                     const Reading* r, Trend* tr) {
  char water[12], raw[12], uncomp[12], comp[12];
  waterCell(ds, t, water, sizeof(water));

  const bool tdsOk = tds->ready && r->hasRaw;
  const char* state = tds->ready ? tdsStateName(r) : "-";
  if (tdsOk) {
    snprintf(raw, sizeof(raw), "%ld", (long)r->raw);
    snprintf(uncomp, sizeof(uncomp), "%.0f", (double)tdsPpmFromModuleMv((float)r->raw, 1.0f));
  } else {
    snprintf(raw, sizeof(raw), "-");
    snprintf(uncomp, sizeof(uncomp), "-");
  }
  // value is compensated unless the driver flagged it uncompensated.
  const bool compensated = tdsOk && !(r->qualityFlags & kQualityUncompensated);
  if (compensated) {
    snprintf(comp, sizeof(comp), "%.0f", (double)r->value);
  } else {
    snprintf(comp, sizeof(comp), "-");
  }
  const float shown = compensated ? r->value : tdsPpmFromModuleMv((float)r->raw, 1.0f);
  const char* trend = trendUpdate(tr, tdsOk, compensated, shown);

  printf("%7lu | %7s | %6s | %6s | %6s | %-7s | %-8s |\n", (unsigned long)(now / 1000), water,
         raw, uncomp, comp, state, trend);
}

static void waterTask(void* arg) {
  (void)arg;
  DriverRun ds = {&s_ds.base, false, false, false, DRIVER_OK, 0, 0, 0};
  DriverRun tds = {&s_tds.base, false, false, false, DRIVER_OK, 0, 0, 0};
  Trend trend;
  memset(&trend, 0, sizeof(trend));
  bool dsTimed = false;
  bool tdsTimed = false;
  int lastCompensated = -1;  // unknown
  uint32_t rows = 0;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CFG_FRAME_PERIOD_MS));
    const MonoMs frameStart = monoNowMs();

    // Latest DS18B20 result (previous frame) -> TDS compensation input. A
    // failed or missing reading is passed as is: the TDS driver rejects it.
    Reading water;
    sensorDriverReadResult(&s_ds.base, &water, 1);
    tdsDriverSetWaterTemperature(&s_tds, &water);
    const bool fresh = readingIsActionableAt(&water, frameStart, CFG_STALE_ORDINARY_MS);
    if ((int)fresh != lastCompensated) {
      printf(fresh ? "# tds compensation ON (from DS18B20)\n"
                   : "# DS18B20 not fresh -> tds uncompensated (NO TEMP)\n");
      lastCompensated = fresh;
    }

    beginRun(&ds, monoNowMs());
    beginRun(&tds, monoNowMs());
    while (ds.pending || tds.pending) {
      MonoMs now = monoNowMs();
      if (now - frameStart >= CFG_FRAME_PERIOD_MS) {
        cancelRun(&ds);
        cancelRun(&tds);
        break;
      }
      stepRun(&ds, now);
      stepRun(&tds, monoNowMs());

      // Sleep until the earlier of the two next polls.
      now = monoNowMs();
      MonoMs wake = now + 1;
      bool any = false;
      if (ds.pending) { wake = ds.nextPollMs; any = true; }
      if (tds.pending && (!any || tds.nextPollMs < wake)) wake = tds.nextPollMs;
      const uint32_t waitMs = wake > now ? (uint32_t)(wake - now) : 1;
      const TickType_t ticks = pdMS_TO_TICKS(waitMs);
      vTaskDelay(ticks > 0 ? ticks : 1);
    }

    printRomIfNew();
    printTimingOnce(&ds, &dsTimed);
    printTimingOnce(&tds, &tdsTimed);
    Reading t;
    Reading ppm;
    sensorDriverReadResult(&s_ds.base, &t, 1);
    sensorDriverReadResult(&s_tds.base, &ppm, 1);
    if (rows++ % WATER_HEADER_EVERY == 0) printHeader();
    printRow(monoNowMs(), &ds, &t, &tds, &ppm, &trend);
  }
}

void setup(void) {
  ds18b20DriverConstruct(&s_ds, &s_dsConfig);
  tdsDriverConstruct(&s_tds, &s_tdsConfig);
  delay(200);
  printf("\n# water bring-up: DS18B20 GPIO%d + TDS GPIO%d, %lu ms frame\n",
         (int)BOARD_PIN_ONE_WIRE, (int)BOARD_PIN_TDS_ADC, (unsigned long)CFG_FRAME_PERIOD_MS);
  if (!oneWireBusBegin(&s_oneWire, BOARD_PIN_ONE_WIRE)) {
    printf("# GPIO%d cannot be used for 1-Wire\n", (int)BOARD_PIN_ONE_WIRE);
  }
  if (!adcChannelBegin(&s_adc, BOARD_PIN_TDS_ADC)) {
    printf("# GPIO%d is not an ADC1 pin\n", (int)BOARD_PIN_TDS_ADC);
  }

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_ACQUISITION_NORMAL);
  xTaskCreatePinnedToCore(waterTask, "water", spec->stackBytes, NULL, spec->priority, NULL,
                          spec->core);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

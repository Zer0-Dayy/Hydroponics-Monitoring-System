// Minimal RTOS bring-up harness shared by every test/bringup/<sensor>_main.c.
// Architecture §17 stage 1-3, requirement R9.
//
// Runs ONE production driver from a task pinned like AcquisitionNormal
// (core 1, priority 4), on an absolute period (vTaskDelayUntil). Each period:
// init (retried until OK) -> start -> poll until READY/error -> readResult.
// Prints every reading with its health and the start->ready duration so the
// conversion budget in app_config.h can be checked against reality.
#pragma once
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "domain/reading.h"
#include "drivers/sensor_driver.h"
#include "hal/arduino_entry.h"
#include "hal/clock.h"

typedef struct BringupContext {
  SensorDriver* driver;
  uint32_t periodMs;
  // Optional, called from the harness task after every READY result (so it
  // may use driver getters without breaking single bus ownership). NULL = none.
  void (*afterResult)(SensorDriver* driver);
} BringupContext;

// One compact line per measurement, e.g.
//   [bme280] 27.98 degC | 54.20 %RH | 101602.04 Pa   (11 ms)
// An UNCALIBRATED reading shows its raw-unit value with "(uncal)", e.g.
//   [ph4502c] 2498.31 mV (uncal)   (271 ms)
// Any other non-OK reading shows its metric and health instead of a value.
static inline void bringupPrintResult(const char* name, const Reading* r, uint8_t n,
                                      uint32_t elapsedMs) {
  printf("[%s] ", name);
  for (uint8_t i = 0; i < n; ++i) {
    if (i > 0) printf(" | ");
    if (r[i].health == HEALTH_OK) {
      printf("%.2f %s", (double)r[i].value, unitName(r[i].unit));
    } else if (r[i].health == HEALTH_UNCALIBRATED) {
      printf("%.2f %s (uncal)", (double)r[i].value, unitName(r[i].unit));
    } else {
      printf("%s=%s", metricName(r[i].metric), healthName(r[i].health));
      if (r[i].hasRaw) printf(" (raw %ld)", (long)r[i].raw);
    }
  }
  printf("   (%lu ms)\n", (unsigned long)elapsedMs);
}

static inline void bringupTask(void* arg) {
  BringupContext* ctx = (BringupContext*)arg;
  SensorDriver* drv = ctx->driver;
  bool initialised = false;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(ctx->periodMs));

    if (!initialised) {
      DriverStatus st = sensorDriverInit(drv);
      printf("[%s] init -> %s\n", sensorDriverName(drv), driverStatusName(st));
      if (st != DRIVER_OK) continue;
      initialised = true;
    }

    const MonoMs t0 = monoNowMs();
    DriverStatus st = sensorDriverStart(drv, t0);
    if (st != DRIVER_OK) {
      printf("[%s] start -> %s\n", sensorDriverName(drv), driverStatusName(st));
      if (st != DRIVER_PENDING) initialised = false;
      continue;
    }

    PollResult pr = sensorDriverPoll(drv, monoNowMs());
    while (pr.status == DRIVER_PENDING) {
      const MonoMs now = monoNowMs();
      if (now - t0 >= ctx->periodMs) {  // harness-level guard, not the driver budget
        sensorDriverCancel(drv);
        pr.status = DRIVER_TIMEOUT;
        break;
      }
      uint32_t waitMs = 1;
      if (pr.nextPollMs > now) waitMs = (uint32_t)(pr.nextPollMs - now);
      TickType_t ticks = pdMS_TO_TICKS(waitMs);
      vTaskDelay(ticks > 0 ? ticks : 1);
      pr = sensorDriverPoll(drv, monoNowMs());
    }

    const uint32_t elapsedMs = (uint32_t)(monoNowMs() - t0);
    if (pr.status != DRIVER_READY) {
      printf("[%s] poll -> %s after %lu ms\n", sensorDriverName(drv),
             driverStatusName(pr.status), (unsigned long)elapsedMs);
      if (pr.status == DRIVER_NOT_IMPLEMENTED || pr.status == DRIVER_NOT_PRESENT) {
        initialised = false;
      }
      continue;
    }

    Reading out[4];
    const uint8_t n = sensorDriverReadResult(drv, out, 4);
    bringupPrintResult(sensorDriverName(drv), out, n, elapsedMs);
    if (ctx->afterResult != NULL) ctx->afterResult(drv);
  }
}

// Start the harness task with the AcquisitionNormal placement from §6.
static inline void bringupStart(BringupContext* ctx) {
  const TaskSpec* spec = cfgTaskSpec(TASK_ID_ACQUISITION_NORMAL);
  xTaskCreatePinnedToCore(bringupTask, "bringup", spec->stackBytes, ctx,
                          spec->priority, NULL, spec->core);
}

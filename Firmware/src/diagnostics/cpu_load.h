// CPU load per core, for the app's hardware diagnostics (get_diagnostics).
//
// FreeRTOS run-time stats are off in the Arduino prebuilt sdkconfig, so the
// load is measured from the idle side: each core's idle hook executes the
// WAITI itself (what the IDF idle hook would do next anyway), counts the
// cycles spent halted and returns false so IDF does not halt a second time.
// The CPU sleeps exactly as before: nothing busy-waits and nothing extra runs
// while a task is ready. Interrupt handlers count as busy time.
//
// Supervisor calls cpuLoadSample() once per second; load = 100 - halted share.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CPU_LOAD_CORES 2
#define CPU_LOAD_PEAK_WINDOW_S 60

typedef struct CpuLoad {
  uint8_t loadPct;  // over the last sample period
  uint8_t peakPct;  // highest loadPct over the last CPU_LOAD_PEAK_WINDOW_S samples
} CpuLoad;

bool cpuLoadInit(void);  // registers the idle hooks; false if a hook slot is missing
// Call from one task only, about once per second. Returns false until two
// samples exist (the first one only sets the baseline).
bool cpuLoadSample(CpuLoad out[CPU_LOAD_CORES]);

// Pure: load in percent from the halted and elapsed time of one period.
uint8_t cpuLoadPercent(uint32_t idleUs, uint32_t elapsedUs);

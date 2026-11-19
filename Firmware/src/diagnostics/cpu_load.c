#include "diagnostics/cpu_load.h"

#include <esp32-hal-cpu.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>
#include <hal/cpu_hal.h>
#include <string.h>

// Cycles each core spent halted in its idle hook. Written only by that core's
// idle task; an aligned 32-bit read is atomic, and unsigned deltas make the
// wrap (about 17.9 s at 240 MHz) harmless while samples come every second.
static volatile uint32_t s_idleCycles[CPU_LOAD_CORES];

static uint32_t s_lastCycles[CPU_LOAD_CORES];
static int64_t s_lastUs = 0;
static uint32_t s_cyclesPerUs = 240;
static uint8_t s_history[CPU_LOAD_CORES][CPU_LOAD_PEAK_WINDOW_S];
static uint8_t s_historyNext = 0;
static uint8_t s_historyCount = 0;

static inline void haltCounted(int core) {
  const uint32_t start = cpu_hal_get_cycle_count();
  cpu_hal_waiti();  // returns on the next interrupt (tick or device)
  s_idleCycles[core] += cpu_hal_get_cycle_count() - start;
}

// false: IDF then skips its own WAITI, so the core halts once per pass, here.
static bool idleHookCore0(void) {
  haltCounted(0);
  return false;
}

static bool idleHookCore1(void) {
  haltCounted(1);
  return false;
}

uint8_t cpuLoadPercent(uint32_t idleUs, uint32_t elapsedUs) {
  if (elapsedUs == 0) return 0;
  if (idleUs >= elapsedUs) return 0;
  const uint32_t busy = elapsedUs - idleUs;
  return (uint8_t)(((uint64_t)busy * 100u + elapsedUs / 2) / elapsedUs);
}

bool cpuLoadInit(void) {
  // Constant: dynamic frequency scaling is off (CONFIG_PM_ENABLE not set).
  const uint32_t mhz = getCpuFrequencyMhz();
  if (mhz > 0) s_cyclesPerUs = mhz;
  memset(s_history, 0, sizeof(s_history));
  const bool ok0 = esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0) == ESP_OK;
  const bool ok1 = esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1) == ESP_OK;
  return ok0 && ok1;
}

bool cpuLoadSample(CpuLoad out[CPU_LOAD_CORES]) {
  const int64_t now = esp_timer_get_time();
  uint32_t cycles[CPU_LOAD_CORES];
  for (int c = 0; c < CPU_LOAD_CORES; ++c) cycles[c] = s_idleCycles[c];
  const bool first = s_lastUs == 0;
  const int64_t elapsed = now - s_lastUs;
  s_lastUs = now;
  uint32_t idle[CPU_LOAD_CORES];
  for (int c = 0; c < CPU_LOAD_CORES; ++c) {
    idle[c] = (cycles[c] - s_lastCycles[c]) / s_cyclesPerUs;
    s_lastCycles[c] = cycles[c];
  }
  if (first || elapsed <= 0 || elapsed > 15 * 1000 * 1000) return false;  // no baseline, or a wrapped counter

  for (int c = 0; c < CPU_LOAD_CORES; ++c) {
    s_history[c][s_historyNext] = cpuLoadPercent(idle[c], (uint32_t)elapsed);
  }
  s_historyNext = (uint8_t)((s_historyNext + 1) % CPU_LOAD_PEAK_WINDOW_S);
  if (s_historyCount < CPU_LOAD_PEAK_WINDOW_S) ++s_historyCount;
  const uint8_t latest = (uint8_t)((s_historyNext + CPU_LOAD_PEAK_WINDOW_S - 1) % CPU_LOAD_PEAK_WINDOW_S);
  for (int c = 0; c < CPU_LOAD_CORES; ++c) {
    uint8_t peak = 0;
    for (uint8_t i = 0; i < s_historyCount; ++i) {
      if (s_history[c][i] > peak) peak = s_history[c][i];
    }
    out[c].loadPct = s_history[c][latest];
    out[c].peakPct = peak;
  }
  return true;
}

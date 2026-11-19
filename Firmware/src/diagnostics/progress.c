#include "diagnostics/progress.h"

#include <freertos/FreeRTOS.h>

#include "hal/clock.h"

// 64-bit stamps are written as two 32-bit halves: a reader on the other core
// could see a torn value without the lock (§11).
// The acquisition worker without the lease marks nothing; Supervisor checks
// only the lease holder (acquisitionActiveTaskId(), WP9).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static MonoMs s_lastMs[TASK_ID_COUNT];

void progressMark(TaskId task) {
  if (task >= TASK_ID_COUNT) return;
  const MonoMs now = monoNowMs();
  portENTER_CRITICAL(&s_mux);
  s_lastMs[task] = now;
  portEXIT_CRITICAL(&s_mux);
}

MonoMs progressLastMs(TaskId task) {
  if (task >= TASK_ID_COUNT) return 0;
  portENTER_CRITICAL(&s_mux);
  const MonoMs t = s_lastMs[task];
  portEXIT_CRITICAL(&s_mux);
  return t;
}

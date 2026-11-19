#include "diagnostics/logger.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>

#include "app_config.h"
#include "diagnostics/progress.h"

typedef struct LogRecord {
  LogLevel level;
  char text[LOG_TEXT_LEN];
} LogRecord;

static StaticQueue_t s_queueControl;
static uint8_t s_queueStorage[CFG_LOG_QUEUE_LEN * sizeof(LogRecord)];
static QueueHandle_t s_queue = NULL;

static StaticTask_t s_tcb;
static StackType_t s_stack[2 * 1024];  // cfgTaskSpec(TASK_ID_SERIAL_LOGGER)->stackBytes
static TaskHandle_t s_task = NULL;

static portMUX_TYPE s_dropMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped = 0;

static const char* tagOf(LogLevel level) {
  static const char* const kTags[] = {"E", "W", "I", "D"};
  return level < sizeof(kTags) / sizeof(kTags[0]) ? kTags[level] : "?";
}

static void serialLoggerTask(void* arg) {
  (void)arg;
  static LogRecord rec;  // single consumer: static keeps it off the 2 KiB stack
  uint32_t reportedDrops = 0;
  for (;;) {
    if (xQueueReceive(s_queue, &rec, portMAX_DELAY) != pdTRUE) continue;
    fputs("[", stdout);
    fputs(tagOf(rec.level), stdout);
    fputs("] ", stdout);
    fputs(rec.text, stdout);
    fputs("\n", stdout);
    const uint32_t dropped = logDroppedCount();
    if (dropped != reportedDrops) {  // say so in the log itself, once per change
      printf("[W] logger: %lu records dropped (queue full)\n", (unsigned long)dropped);
      reportedDrops = dropped;
    }
    progressMark(TASK_ID_SERIAL_LOGGER);
  }
}

uint32_t logQueueDepth(void) { return s_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0; }

void* logTaskHandle(void) { return s_task; }

void logInit(void) {
  if (s_task != NULL) return;
  s_queue = xQueueCreateStatic(CFG_LOG_QUEUE_LEN, sizeof(LogRecord), s_queueStorage, &s_queueControl);
  if (s_queue == NULL) return;  // logPrintf keeps printing directly
  const TaskSpec* spec = cfgTaskSpec(TASK_ID_SERIAL_LOGGER);
  s_task = xTaskCreateStaticPinnedToCore(serialLoggerTask, spec->name, sizeof(s_stack), NULL,
                                         spec->priority, s_stack, &s_tcb, spec->core);
}

void logPrintf(LogLevel level, const char* fmt, ...) {
  LogRecord rec;
  rec.level = level;
  va_list args;
  va_start(args, fmt);
  vsnprintf(rec.text, sizeof(rec.text), fmt, args);
  va_end(args);

  if (s_task == NULL) {  // early boot or bring-up: direct, blocking write
    printf("[%s] %s\n", tagOf(level), rec.text);
    return;
  }
  if (xQueueSend(s_queue, &rec, 0) != pdTRUE) {
    portENTER_CRITICAL(&s_dropMux);
    ++s_dropped;
    portEXIT_CRITICAL(&s_dropMux);
  }
}

uint32_t logDroppedCount(void) {
  portENTER_CRITICAL(&s_dropMux);
  const uint32_t n = s_dropped;
  portEXIT_CRITICAL(&s_dropMux);
  return n;
}

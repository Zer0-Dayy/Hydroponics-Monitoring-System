#include "services/snapshot_store.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static StaticQueue_t s_queueControl;
static uint8_t s_queueStorage[sizeof(Snapshot)];
static QueueHandle_t s_queue = NULL;

bool snapshotStoreInit(void) {
  if (s_queue == NULL) {
    s_queue = xQueueCreateStatic(1, sizeof(Snapshot), s_queueStorage, &s_queueControl);
  }
  return s_queue != NULL;
}

void snapshotPublish(const Snapshot* snapshot) {
  if (s_queue != NULL) xQueueOverwrite(s_queue, snapshot);
}

bool snapshotPeek(Snapshot* out) {
  if (s_queue == NULL || out == NULL) return false;
  return xQueuePeek(s_queue, out, 0) == pdTRUE;
}

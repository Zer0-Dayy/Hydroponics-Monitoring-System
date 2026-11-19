#include "services/flash_gate.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static StaticSemaphore_t s_gateControl;
static SemaphoreHandle_t s_gate = NULL;
static bool s_pingHeld;  // touched by acquisition only

bool flashGateInit(void) {
  if (s_gate == NULL) s_gate = xSemaphoreCreateMutexStatic(&s_gateControl);
  return s_gate != NULL;
}

bool flashGateTryTakeForPing(void) {
  if (s_gate == NULL) return true;
  if (s_pingHeld) return true;
  s_pingHeld = xSemaphoreTake(s_gate, 0) == pdTRUE;
  return s_pingHeld;
}

void flashGateGiveAfterPing(void) {
  if (s_gate == NULL || !s_pingHeld) return;
  s_pingHeld = false;
  xSemaphoreGive(s_gate);
}

bool flashGateTakeForWrite(uint32_t waitMs) {
  if (s_gate == NULL) return true;
  return xSemaphoreTake(s_gate, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void flashGateGiveAfterWrite(void) {
  if (s_gate != NULL) xSemaphoreGive(s_gate);
}

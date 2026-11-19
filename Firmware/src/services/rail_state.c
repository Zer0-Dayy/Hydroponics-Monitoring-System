#include "services/rail_state.h"

#include <freertos/FreeRTOS.h>

static portMUX_TYPE s_railMux = portMUX_INITIALIZER_UNLOCKED;
// Bench bring-up assumes the 5 V rail is present. Otherwise unknown power at
// boot is treated conservatively as unavailable (§11) until WP9 samples it.
#ifdef FW_BRINGUP
static RailState s_rail = {true, 1};
#else
static RailState s_rail = {false, 1};
#endif

RailState railStateRead(void) {
  RailState copy;
  portENTER_CRITICAL(&s_railMux);
  copy = s_rail;
  portEXIT_CRITICAL(&s_railMux);
  return copy;
}

void railStateInvalidateFromIsr(void) {
  portENTER_CRITICAL_ISR(&s_railMux);
  s_rail.valid5v = false;
  s_rail.epoch++;
  portEXIT_CRITICAL_ISR(&s_railMux);
}

void railStateInvalidate(void) {
  portENTER_CRITICAL(&s_railMux);
  s_rail.valid5v = false;
  s_rail.epoch++;
  portEXIT_CRITICAL(&s_railMux);
}

void railStateRestore(uint32_t expectedEpoch) {
  portENTER_CRITICAL(&s_railMux);
  if (s_rail.epoch == expectedEpoch) s_rail.valid5v = true;
  portEXIT_CRITICAL(&s_railMux);
}

#include "hal/clock.h"

#include <esp_timer.h>

MonoMs monoNowMs(void) { return (MonoMs)(esp_timer_get_time() / 1000); }

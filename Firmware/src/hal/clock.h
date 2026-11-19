// Monotonic clock (HAL). The only source of "now" for ages and deadlines.
#pragma once
#include "domain/types.h"

MonoMs monoNowMs(void);

// Exponential retry backoff with jitter (§17: 1, 2, 4, 8 s ... up to 60 s).
// Pure C; the caller supplies the random number. WP8 (Wi-Fi reconnect).
#pragma once
#include <stdint.h>

typedef struct Backoff {
  uint32_t initialMs;
  uint32_t maxMs;
  uint32_t currentMs;   // base delay of the next attempt
  uint8_t jitterPct;    // +/- percentage applied to each delay
} Backoff;

void backoffInit(Backoff* b, uint32_t initialMs, uint32_t maxMs, uint8_t jitterPct);

// Back to initialMs (after a successful connection).
void backoffReset(Backoff* b);

// Delay before the next attempt: currentMs +/- jitter, never above maxMs and
// never 0. Then the base doubles, capped at maxMs.
uint32_t backoffNext(Backoff* b, uint32_t random);

#include "domain/backoff.h"

void backoffInit(Backoff* b, uint32_t initialMs, uint32_t maxMs, uint8_t jitterPct) {
  b->initialMs = initialMs == 0 ? 1 : initialMs;
  b->maxMs = maxMs < b->initialMs ? b->initialMs : maxMs;
  b->jitterPct = jitterPct > 100 ? 100 : jitterPct;
  b->currentMs = b->initialMs;
}

void backoffReset(Backoff* b) { b->currentMs = b->initialMs; }

uint32_t backoffNext(Backoff* b, uint32_t random) {
  const uint32_t base = b->currentMs;
  const uint32_t span = (uint32_t)(((uint64_t)base * b->jitterPct) / 100u);
  uint64_t delay = (uint64_t)base - span + (span > 0 ? random % (2u * (uint64_t)span + 1u) : 0u);
  if (delay > b->maxMs) delay = b->maxMs;
  if (delay == 0) delay = 1;

  const uint64_t doubled = (uint64_t)base * 2u;
  b->currentMs = doubled > b->maxMs ? b->maxMs : (uint32_t)doubled;
  return (uint32_t)delay;
}

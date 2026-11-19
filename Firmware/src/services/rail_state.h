// 5 V rail validity + epoch (§12). Work package WP9 (Stage 6).
// The ISR invalidates and bumps the epoch on every sense edge; only
// PowerCoordinator may restore validity. Copies are taken in a short
// portMUX critical section so both cores see a coherent {valid, epoch}.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct RailState {
  bool valid5v;
  uint32_t epoch;
} RailState;

RailState railStateRead(void);
void railStateInvalidateFromIsr(void);          // ISR-safe
void railStateInvalidate(void);                 // task context (PowerCoordinator polling fallback)
void railStateRestore(uint32_t expectedEpoch);  // PowerCoordinator only

// Accept a 5 V result only if the rail was valid before and after and the
// epoch did not change in between.
static inline bool railSampleStillValid(const RailState* before, const RailState* after) {
  return before->valid5v && after->valid5v && before->epoch == after->epoch;
}

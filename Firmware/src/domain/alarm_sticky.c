#include "domain/alarm_sticky.h"

static bool moreExtreme(const AlarmRule* rule, float a, float b) {
  return rule->comparison == COMPARISON_ABOVE ? a > b : a < b;
}

void stickyBegin(StickySlot* slot, const AlarmRule* rule, const AlarmEvent* e) {
  slot->last = e->decision;
  if (e->decision == RULE_DECISION_TRIP) {
    if (!slot->tripSinceDrain || moreExtreme(rule, e->sample.value, slot->trip.sample.value)) slot->trip = *e;
    slot->tripSinceDrain = true;
  } else {
    slot->clear = *e;
  }
  if (slot->inFlight < UINT8_MAX) ++slot->inFlight;
}

void stickyEnd(StickySlot* slot, bool queued) {
  if (!queued) slot->lost = true;
  if (slot->inFlight > 0) --slot->inFlight;
}

uint8_t stickyCollect(StickySlot* slots, uint8_t count, const bool* sawTrip, AlarmEvent* out) {
  uint8_t n = 0;
  for (uint8_t r = 0; r < count; ++r) {
    StickySlot* slot = &slots[r];
    if (slot->inFlight > 0) continue;  // outcome unknown: keep everything for the next pass
    if (slot->lost) {
      if (slot->tripSinceDrain && !sawTrip[r]) out[n++] = slot->trip;
      if (slot->last == RULE_DECISION_CLEAR) out[n++] = slot->clear;
    }
    slot->tripSinceDrain = false;
    slot->lost = false;
  }
  return n;
}

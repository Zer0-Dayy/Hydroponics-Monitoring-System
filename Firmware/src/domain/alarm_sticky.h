// Sticky per-rule fallback for the alarm detail queue (§11). Pure C, no
// locking: services/safety.c wraps every call in its portMUX and does the
// queue send between stickyBegin() and stickyEnd().
//
// Producer (acquisition), per event:
//   stickyBegin()   record the decision, mark the slot in flight
//   xQueueSend()    outside the critical section
//   stickyEnd()     mark the record lost if the send failed, end the flight
// Consumer (Safety), per pass: drain the queue, then stickyCollect().
//
// A slot in flight is left untouched by stickyCollect(): its send result is
// not known yet, so its flags must survive until stickyEnd(). The producer
// wakes Safety after stickyEnd(), so the next pass sees the outcome. Without
// this, a drain between a failed send and the "lost" mark erased the TRIP
// from both delivery paths.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/alarm.h"

typedef struct StickySlot {
  bool tripSinceDrain;  // a TRIP was submitted since Safety last collected
  bool lost;            // a detail record of this rule was dropped
  uint8_t inFlight;     // submits between stickyBegin() and stickyEnd()
  RuleDecision last;    // last decision submitted (TRIP or CLEAR)
  AlarmEvent trip;      // worst TRIP since the last collect
  AlarmEvent clear;     // the last CLEAR
} StickySlot;

void stickyBegin(StickySlot* slot, const AlarmRule* rule, const AlarmEvent* e);
void stickyEnd(StickySlot* slot, bool queued);

// After the queue was drained: for each slot (not in flight) whose detail
// record was dropped, append the TRIP if none came through the queue
// (sawTrip[rule]) and, if the rule's last decision was CLEAR, that CLEAR
// too. Writes at most 2 * count events at out[0..]; returns how many.
uint8_t stickyCollect(StickySlot* slots, uint8_t count, const bool* sawTrip, AlarmEvent* out);

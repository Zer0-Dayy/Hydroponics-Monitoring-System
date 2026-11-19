// Alarm event journal with independent reader cursors (§11, RTOS §14.1).
// Pure C, no locking: services/safety.c wraps writes and reads in a portMUX.
// Work package WP8 (Stage 5).
//
// Safety appends; BLE and LAN each keep their own cursor, so one slow or
// disconnected link never holds events back from the other. When a reader
// falls more than EVENT_JOURNAL_LEN events behind, the oldest details are
// lost and the next read reports a gap (drop oldest, with gap indication).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/alarm.h"

#define EVENT_JOURNAL_LEN 64  // = CFG_ALARM_JOURNAL_LEN (checked in services/safety.c)

typedef uint8_t JournalKind;
enum { JOURNAL_KIND_TRIP = 0, JOURNAL_KIND_CLEAR, JOURNAL_KIND_RESET };

typedef struct JournalEvent {
  uint32_t n;          // 1, 2, 3, ... assigned on append
  uint8_t ruleId;
  Severity severity;
  JournalKind kind;
  MonoMs atMs;         // sample end time (trip/clear) or reset time
  float value;
} JournalEvent;

typedef struct EventJournal {
  JournalEvent ring[EVENT_JOURNAL_LEN];
  uint32_t nextN;      // n the next append gets
} EventJournal;

typedef struct JournalCursor {
  uint32_t nextN;      // next event this reader wants
} JournalCursor;

void journalInit(EventJournal* j);

// Stores a copy of *e with a new n; returns that n.
uint32_t journalAppend(EventJournal* j, const JournalEvent* e);

// A new reader starts at the next event (it does not replay history).
void journalCursorInit(const EventJournal* j, JournalCursor* c);

// Next event for this reader, false if none. *gap is true when events were
// lost before this one (the cursor jumps to the oldest one still stored).
bool journalNext(const EventJournal* j, JournalCursor* c, JournalEvent* out, bool* gap);

const char* journalKindName(JournalKind k);
const char* severityName(Severity s);

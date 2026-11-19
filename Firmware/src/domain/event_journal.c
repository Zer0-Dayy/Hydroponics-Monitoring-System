#include "domain/event_journal.h"

#include <string.h>

void journalInit(EventJournal* j) {
  memset(j, 0, sizeof(*j));
  j->nextN = 1;
}

uint32_t journalAppend(EventJournal* j, const JournalEvent* e) {
  const uint32_t n = j->nextN++;
  JournalEvent* slot = &j->ring[n % EVENT_JOURNAL_LEN];
  *slot = *e;
  slot->n = n;
  return n;
}

void journalCursorInit(const EventJournal* j, JournalCursor* c) { c->nextN = j->nextN; }

static uint32_t oldestStored(const EventJournal* j) {
  return j->nextN > EVENT_JOURNAL_LEN ? j->nextN - EVENT_JOURNAL_LEN : 1;
}

bool journalNext(const EventJournal* j, JournalCursor* c, JournalEvent* out, bool* gap) {
  *gap = false;
  if (c->nextN >= j->nextN) return false;
  const uint32_t oldest = oldestStored(j);
  if (c->nextN < oldest) {
    *gap = true;
    c->nextN = oldest;
  }
  *out = j->ring[c->nextN % EVENT_JOURNAL_LEN];
  ++c->nextN;
  return true;
}

const char* journalKindName(JournalKind k) {
  switch (k) {
    case JOURNAL_KIND_TRIP: return "trip";
    case JOURNAL_KIND_CLEAR: return "clear";
    case JOURNAL_KIND_RESET: return "reset";
  }
  return "?";
}

const char* severityName(Severity s) {
  switch (s) {
    case SEVERITY_INFO: return "INFO";
    case SEVERITY_WARNING: return "WARNING";
    case SEVERITY_CRITICAL: return "CRITICAL";
  }
  return "?";
}

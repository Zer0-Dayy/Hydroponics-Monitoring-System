// Absolute-deadline job table for the acquisition engine (§9). Pure C.
// Work package WP7 (Stage 4). Host-tested; the acquisition task only calls it.
//
// Each job has a period and an absolute next deadline on the monotonic clock.
// A job that runs late keeps its grid: the next deadline is the first grid
// point after `now`. Missed grid points are counted as skipped and are never
// replayed in a burst ("skip, never catch up").
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "domain/types.h"

// Lateness up to this is release jitter, not a late run (§19: <= 5 ms).
#define SCHEDULE_LATE_TOLERANCE_MS ((uint32_t)5)

typedef struct ScheduleJob {
  uint32_t periodMs;
  uint32_t budgetMs;   // worst-case worker time for one step of this job
  MonoMs nextDueMs;
  uint32_t runs;
  uint32_t late;       // runs started more than SCHEDULE_LATE_TOLERANCE_MS late
  uint32_t skipped;    // whole periods that passed without a run
  uint32_t maxLateMs;  // largest lateness seen (jitter for on-time runs)
  uint32_t lastLateMs;
} ScheduleJob;

void scheduleJobInit(ScheduleJob* job, uint32_t periodMs, uint32_t budgetMs, MonoMs firstDueMs);

static inline bool scheduleJobDue(const ScheduleJob* job, MonoMs now) {
  return monoReached(now, job->nextDueMs);
}

// Record a run that starts at `now` (call only when due) and move the
// deadline to the next grid point strictly after `now`.
void scheduleJobMarkRun(ScheduleJob* job, MonoMs now);

// Earliest deadline over `count` jobs (count >= 1).
MonoMs scheduleEarliestDue(const ScheduleJob* jobs, uint8_t count);

// true if a step of `budgetMs` started at `now` ends no later than `deadline`
// (§9: defer an ordinary step that would cross the next BME280 slot).
static inline bool scheduleFitsBefore(MonoMs now, uint32_t budgetMs, MonoMs deadline) {
  return now + budgetMs <= deadline;
}

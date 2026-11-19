#include "domain/schedule.h"

void scheduleJobInit(ScheduleJob* job, uint32_t periodMs, uint32_t budgetMs, MonoMs firstDueMs) {
  job->periodMs = periodMs > 0 ? periodMs : 1;
  job->budgetMs = budgetMs;
  job->nextDueMs = firstDueMs;
  job->runs = 0;
  job->late = 0;
  job->skipped = 0;
  job->maxLateMs = 0;
  job->lastLateMs = 0;
}

void scheduleJobMarkRun(ScheduleJob* job, MonoMs now) {
  const MonoMs lateness = now > job->nextDueMs ? now - job->nextDueMs : 0;
  const MonoMs missed = lateness / job->periodMs;  // grid points already passed
  const uint32_t lateMs = lateness > UINT32_MAX ? UINT32_MAX : (uint32_t)lateness;

  ++job->runs;
  job->lastLateMs = lateMs;
  if (lateMs > job->maxLateMs) job->maxLateMs = lateMs;
  if (lateMs > SCHEDULE_LATE_TOLERANCE_MS) ++job->late;
  job->skipped += missed > UINT32_MAX ? UINT32_MAX : (uint32_t)missed;
  job->nextDueMs += (missed + 1) * job->periodMs;
}

MonoMs scheduleEarliestDue(const ScheduleJob* jobs, uint8_t count) {
  MonoMs earliest = jobs[0].nextDueMs;
  for (uint8_t i = 1; i < count; ++i) {
    if (jobs[i].nextDueMs < earliest) earliest = jobs[i].nextDueMs;
  }
  return earliest;
}

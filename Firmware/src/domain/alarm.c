#include "domain/alarm.h"

#include "app_config.h"

uint32_t alarmStaleLimitMs(MetricId metric) {
  switch (metric) {
    case METRIC_AIR_TEMPERATURE:
    case METRIC_AIR_HUMIDITY:
    case METRIC_AIR_PRESSURE: return CFG_STALE_BME280_MS;
    default: return CFG_STALE_ORDINARY_MS;
  }
}

static bool beyondTrip(const AlarmRule* rule, float v) {
  return rule->comparison == COMPARISON_ABOVE ? v >= rule->tripThreshold
                                              : v <= rule->tripThreshold;
}

static bool pastClear(const AlarmRule* rule, float v) {
  return rule->comparison == COMPARISON_ABOVE ? v <= rule->clearThreshold
                                              : v >= rule->clearThreshold;
}

RuleDecision evaluateRule(const AlarmRule* rule, const Reading* sample, MonoMs now,
                          uint32_t staleLimitMs, AlarmRuleState* state) {
  if (!readingIsActionableAt(sample, now, staleLimitMs)) {
    state->pending = false;
    return RULE_DECISION_NOT_ELIGIBLE;
  }
  const float v = sample->value;
  const MonoMs t = sample->intervalEndMs;

  if (state->tripped) {
    if (!pastClear(rule, v)) return RULE_DECISION_NO_CHANGE;
    state->tripped = false;
    return RULE_DECISION_CLEAR;
  }

  if (!beyondTrip(rule, v)) {
    state->pending = false;
    return RULE_DECISION_NO_CHANGE;
  }
  if (!state->pending) {
    state->pending = true;
    state->pendingSinceMs = t;
  }
  if (t - state->pendingSinceMs < rule->persistMs) return RULE_DECISION_NO_CHANGE;
  state->pending = false;
  state->tripped = true;
  return RULE_DECISION_TRIP;
}

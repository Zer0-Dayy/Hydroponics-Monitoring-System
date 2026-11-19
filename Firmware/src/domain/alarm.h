// Alarm rules and events (§10). Pure C.
// Work package WP7 (Stage 4). Thresholds are NOT commissioned: the 40/50 degC
// values in the original request are examples only (§1).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "domain/reading.h"

typedef uint8_t Severity;
enum { SEVERITY_INFO = 0, SEVERITY_WARNING, SEVERITY_CRITICAL };

typedef uint8_t Comparison;
enum { COMPARISON_ABOVE = 0, COMPARISON_BELOW };

typedef uint8_t ActuatorAction;
enum { ACTUATOR_ACTION_NONE = 0, ACTUATOR_ACTION_DEENERGIZE, ACTUATOR_ACTION_ENERGIZE };

// Actuator ids used by AlarmRule.actuatorId (board has one relay, §20).
enum { ACTUATOR_NONE = 0, ACTUATOR_RELAY1 = 1 };

typedef struct AlarmRule {
  uint8_t ruleId;         // index of the rule in its table
  bool enabled;           // false until the limit is commissioned
  MetricId metric;
  Comparison comparison;
  float tripThreshold;
  float clearThreshold;   // hysteresis
  uint32_t persistMs;     // 0 = trip on one valid sample
  Severity severity;
  bool latching;          // requires explicit reset after clearing
  uint8_t actuatorId;
  ActuatorAction action;
} AlarmRule;

typedef uint8_t RuleDecision;
enum {
  RULE_DECISION_NOT_ELIGIBLE = 0,
  RULE_DECISION_NO_CHANGE,
  RULE_DECISION_TRIP,
  RULE_DECISION_CLEAR
};

// Event submitted by acquisition to Safety immediately after a sample (§4).
typedef struct AlarmEvent {
  uint8_t ruleId;
  RuleDecision decision;
  Reading sample;
} AlarmEvent;

// Per-rule evaluation state, owned by the caller (acquisition, one per rule).
typedef struct AlarmRuleState {
  bool tripped;
  bool pending;           // beyond the trip threshold, persistMs not yet elapsed
  MonoMs pendingSinceMs;
} AlarmRuleState;

// Freshness limit for a metric (§10): BME280 metrics 1 s, all others 3 s.
// Used by acquisition when it evaluates rules and by Safety's stale checks.
uint32_t alarmStaleLimitMs(MetricId metric);

// Pure threshold evaluation for one sample. Only `state` is modified.
// A sample counts only if readingIsActionableAt(sample, now, staleLimitMs):
// a stale or non-OK reading is NOT_ELIGIBLE, neither trips nor clears, and
// restarts any persistence window (a gap in valid data is not "sustained").
//
// ABOVE: trip when value >= trip for persistMs (0 = this sample), clear when
// value <= clear. BELOW mirrors it. trip/clear must leave a hysteresis band
// (ABOVE: clear < trip). Persistence is timed on the samples' intervalEndMs.
// Latching is Safety's job: CLEAR is reported even for a latching rule.
RuleDecision evaluateRule(const AlarmRule* rule, const Reading* sample, MonoMs now,
                          uint32_t staleLimitMs, AlarmRuleState* state);

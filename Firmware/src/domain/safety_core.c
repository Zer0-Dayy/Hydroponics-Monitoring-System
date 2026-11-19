#include "domain/safety_core.h"

#include <string.h>

static bool isRelayInterlock(const AlarmRule* rule) {
  return rule->enabled && rule->actuatorId == ACTUATOR_RELAY1 &&
         rule->action == ACTUATOR_ACTION_DEENERGIZE;
}

static bool moreExtreme(const AlarmRule* rule, float candidate, float current) {
  return rule->comparison == COMPARISON_ABOVE ? candidate > current : candidate < current;
}

void safetyCoreInit(SafetyCore* self, const SafetyConfig* config, const AlarmRule* rules,
                    uint8_t ruleCount, MonoMs now) {
  memset(self, 0, sizeof(*self));
  self->config = *config;
  self->rules = rules;
  self->ruleCount = ruleCount > SAFETY_MAX_RULES ? SAFETY_MAX_RULES : ruleCount;
  if (self->rules == NULL) self->ruleCount = 0;
  lampControlInit(&self->lamp, &config->lamp, now);  // boot counts as a switch to OFF
}

// Step 2. Returns true if a relay-interlock rule tripped in this batch.
static bool applyEvents(SafetyCore* self, const SafetyInputs* in) {
  bool trippedNow = false;
  for (uint8_t i = 0; i < in->eventCount; ++i) {
    const AlarmEvent* e = &in->events[i];
    if (e->ruleId >= self->ruleCount) continue;
    const AlarmRule* rule = &self->rules[e->ruleId];
    SafetyRuleState* st = &self->rule[e->ruleId];
    if (e->decision == RULE_DECISION_TRIP) {
      st->active = true;
      if (rule->latching) st->latched = true;
      if (st->trips == 0 || moreExtreme(rule, e->sample.value, st->worst)) {
        st->worst = e->sample.value;
      }
      if (st->trips == 0) st->firstTripMs = e->sample.intervalEndMs;
      if (st->trips < UINT32_MAX) ++st->trips;
      if (isRelayInterlock(rule)) trippedNow = true;
    } else if (e->decision == RULE_DECISION_CLEAR) {
      st->active = false;  // a latched rule stays latched until reset
    }
  }
  return trippedNow;
}

// Step 3 (lamp). Feeds lamp_control once per new light result, forces OFF on
// stale data even when no new result arrives.
static LampReason updateLamp(SafetyCore* self, const SafetyInputs* in) {
  if (in->snap == NULL) return LAMP_REASON_NO_DATA;
  const Reading* light = &in->snap->readings[METRIC_LIGHT_LEVEL];
  if (light->health == HEALTH_STARTING || light->intervalEndMs == 0) {
    if (!self->haveLight) return LAMP_REASON_NO_DATA;  // lamp is OFF since boot
    if (self->lamp.on) lampControlForceOff(&self->lamp, in->now);
    return LAMP_REASON_STALE;
  }

  // Age from intervalEndMs: readingHealthAt() does not age UNCALIBRATED
  // readings, and the light driver publishes UNCALIBRATED mV.
  const MonoMs end = light->intervalEndMs;
  if (in->now > end && in->now - end > self->config.lightStaleMs) {
    if (self->lamp.on) lampControlForceOff(&self->lamp, in->now);
    return LAMP_REASON_STALE;
  }

  if (!self->haveLight || end != self->lastLightEndMs) {
    self->haveLight = true;
    self->lastLightEndMs = end;
    lampControlUpdate(&self->lamp, light, in->now);
  }
  if (self->lamp.faults > 0) return LAMP_REASON_FAULT;
  return self->lamp.waiting ? LAMP_REASON_DWELL : LAMP_REASON_LEVEL;
}

static uint16_t staleRules(const SafetyCore* self, const SafetyInputs* in) {
  uint16_t mask = 0;
  for (uint8_t i = 0; i < self->ruleCount; ++i) {
    const AlarmRule* rule = &self->rules[i];
    if (!rule->enabled) continue;
    // A switched-off input is not monitored, unless its rule is still latched:
    // then the missing data stays visible until the sensor is back.
    if (rule->metric < METRIC_COUNT && sensorMaskHas(in->sensorDisabled, sensorOfMetric(rule->metric)) &&
        !self->rule[i].latched) {
      continue;
    }
    const uint32_t limit = alarmStaleLimitMs(rule->metric);
    if (in->snap == NULL || rule->metric >= METRIC_COUNT ||
        !readingIsActionableAt(&in->snap->readings[rule->metric], in->now, limit)) {
      mask = (uint16_t)(mask | (1u << i));
    }
  }
  return mask;
}

static void applyCommands(SafetyCore* self, const SafetyInputs* in, SafetyOutputs* out) {
  if (in->commands == NULL) return;
  const uint8_t n = in->commandCount > SAFETY_MAX_COMMANDS ? SAFETY_MAX_COMMANDS : in->commandCount;
  for (uint8_t i = 0; i < n; ++i) {
    const SafetyCommand* c = &in->commands[i];
    SafetyCommandResult* r = &out->results[out->resultCount++];
    r->tag = c->tag;
    if (in->now > c->expiresMs) {
      r->outcome = SAFETY_CMD_EXPIRED;
    } else if (c->kind != SAFETY_CMD_RESET_LATCH || c->ruleId >= self->ruleCount) {
      r->outcome = SAFETY_CMD_BAD_RULE;
    } else {
      r->outcome = safetyCoreResetLatch(self, c->ruleId) ? SAFETY_CMD_APPLIED : SAFETY_CMD_STILL_ACTIVE;
    }
  }
}

void safetyCoreStep(SafetyCore* self, const SafetyInputs* in, SafetyOutputs* out) {
  memset(out, 0, sizeof(*out));

  // 1 Power validity (WP9): 5 V readings are already gated on the rail epoch
  // by acquisition (published UNPOWERED, never actionable); here only the
  // actuator on that rail is handled (step 5).

  // 2 Alarm events.
  const bool trippedNow = applyEvents(self, in);

  // 3 Freshness.
  LampReason reason = updateLamp(self, in);
  out->staleMask = staleRules(self, in);

  // 4 Commands.
  applyCommands(self, in, out);

  // 5 Output.
  bool interlock = trippedNow;
  bool inputMissing = false;
  for (uint8_t i = 0; i < self->ruleCount; ++i) {
    const SafetyRuleState* st = &self->rule[i];
    if (st->latched) out->latchedMask = (uint16_t)(out->latchedMask | (1u << i));
    if (st->active) out->activeMask = (uint16_t)(out->activeMask | (1u << i));
    if (!isRelayInterlock(&self->rules[i])) continue;
    if (st->latched || st->active) interlock = true;
    if ((out->staleMask & (1u << i)) != 0) inputMissing = true;  // the rule cannot see a fault now
  }
  const bool hold = interlock || inputMissing || in->railDown;
  if (hold && self->lamp.on) lampControlForceOff(&self->lamp, in->now);

  out->interlock = interlock;
  out->lampWantsOn = self->lamp.on;
  if (interlock) {
    reason = LAMP_REASON_INTERLOCK;
  } else if (inputMissing) {
    reason = LAMP_REASON_INPUT_MISSING;
  } else if (in->railDown) {
    reason = LAMP_REASON_UNPOWERED;
  } else if (!self->config.lampAutoEnabled) {
    reason = LAMP_REASON_DISABLED;
  }
  out->lampReason = reason;
  out->relayOn = self->config.lampAutoEnabled && !hold && self->lamp.on;
}

bool safetyCoreResetLatch(SafetyCore* self, uint8_t ruleId) {
  if (ruleId >= self->ruleCount) return false;
  SafetyRuleState* st = &self->rule[ruleId];
  if (st->active) return false;
  st->latched = false;
  return true;
}

bool safetyRulesBlockSensor(const AlarmRule* rules, uint8_t ruleCount, uint16_t latchedMask,
                            uint16_t activeMask, SensorId sensor) {
  if (rules == NULL) return false;
  const uint16_t held = (uint16_t)(latchedMask | activeMask);
  for (uint8_t i = 0; i < ruleCount && i < 16; ++i) {
    if (!rules[i].enabled || (held & (1u << i)) == 0) continue;
    if (rules[i].metric < METRIC_COUNT && sensorOfMetric(rules[i].metric) == sensor) return true;
  }
  return false;
}

const char* lampReasonName(LampReason r) {
  switch (r) {
    case LAMP_REASON_NO_DATA: return "no data";
    case LAMP_REASON_LEVEL: return "level";
    case LAMP_REASON_DWELL: return "dwell";
    case LAMP_REASON_FAULT: return "fault";
    case LAMP_REASON_STALE: return "stale";
    case LAMP_REASON_INTERLOCK: return "interlock";
    case LAMP_REASON_DISABLED: return "disabled";
    case LAMP_REASON_UNPOWERED: return "no 5 V";
    case LAMP_REASON_INPUT_MISSING: return "input missing";
  }
  return "?";
}

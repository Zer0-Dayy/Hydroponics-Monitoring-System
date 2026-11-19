// Safety decision logic, pure C (§12). Work package WP7 (Stage 4).
// services/safety.c only waits, copies inputs in, calls safetyCoreStep() and
// writes the relay; every decision is here so it is host-tested with a fake
// clock.
//
// One step = one Safety wake, in the §12 order:
//   1 power validity      (always valid until WP9)
//   2 alarm events        TRIP latches (latching rules) / sets active; CLEAR
//                         releases only non-latching rules
//   3 freshness           new light result -> lamp_control; light result
//                         older than lightStaleMs -> lamp forced OFF; stale
//                         inputs of enabled rules are reported
//   4 commands            WP8: latch resets, refused when expired or while
//                         the rule is still active (§12 step 4)
//   5 output              relay = auto enabled AND lamp ON AND no interlock
//                         AND every relay-interlock input available
// Interlock: any enabled rule with action DEENERGIZE on ACTUATOR_RELAY1 that
// is latched or active, or that tripped in this step's events (so a TRIP
// followed by a CLEAR in the same batch still de-energises once).
// Unavailable input (§12): an enabled relay-interlock rule whose input is in
// staleMask cannot watch the relay, so the relay is held OFF (reason
// INPUT_MISSING) until its data is fresh again. A rule whose sensor was
// switched off is not monitored (staleMask), unless it is still latched.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/alarm.h"
#include "domain/lamp_control.h"
#include "domain/reading.h"
#include "domain/sensor_set.h"

#define SAFETY_MAX_RULES 8
#define SAFETY_MAX_COMMANDS 4

typedef uint8_t LampReason;
enum {
  LAMP_REASON_NO_DATA = 0,  // no light result yet: lamp stays OFF
  LAMP_REASON_LEVEL,        // following the light level (ON or OFF)
  LAMP_REASON_DWELL,        // level asks for a change, dwell still holding
  LAMP_REASON_FAULT,        // unusable light results
  LAMP_REASON_STALE,        // light result too old: forced OFF
  LAMP_REASON_INTERLOCK,    // alarm interlock: relay de-energised
  LAMP_REASON_DISABLED,     // POLICY_LAMP_AUTO_ENABLED = 0
  LAMP_REASON_UNPOWERED,    // WP9: external 5 V down, the relay coil has no supply
  LAMP_REASON_INPUT_MISSING // a relay-interlock rule has no fresh input: relay held OFF
};

typedef struct SafetyConfig {
  bool lampAutoEnabled;
  LampControlConfig lamp;
  uint32_t lightStaleMs;
} SafetyConfig;

typedef struct SafetyRuleState {
  bool active;        // last decision was TRIP and no CLEAR since
  bool latched;       // latching rule tripped; only safetyCoreResetLatch() clears
  uint32_t trips;     // TRIP events seen
  float worst;        // most extreme tripping value
  MonoMs firstTripMs; // sample time of the first TRIP (0 = never)
} SafetyRuleState;

typedef struct SafetyCore {
  SafetyConfig config;
  const AlarmRule* rules;
  uint8_t ruleCount;
  SafetyRuleState rule[SAFETY_MAX_RULES];
  LampControl lamp;
  bool haveLight;
  MonoMs lastLightEndMs;  // intervalEndMs of the last light result fed to lamp
} SafetyCore;

typedef uint8_t SafetyCommandKind;
enum { SAFETY_CMD_RESET_LATCH = 0 };

// A bounded remote request (WP8). The transport sets the expiry; a command
// that waited too long is refused, never applied late (§11, §12).
typedef struct SafetyCommand {
  SafetyCommandKind kind;
  uint8_t ruleId;
  MonoMs expiresMs;
  uint32_t tag;               // caller's request handle, echoed in the result
} SafetyCommand;

typedef uint8_t SafetyCommandOutcome;
enum {
  SAFETY_CMD_APPLIED = 0,
  SAFETY_CMD_EXPIRED,
  SAFETY_CMD_STILL_ACTIVE,    // condition persists: no valid recovery yet
  SAFETY_CMD_BAD_RULE
};

typedef struct SafetyCommandResult {
  uint32_t tag;
  SafetyCommandOutcome outcome;
} SafetyCommandResult;

typedef struct SafetyInputs {
  const Snapshot* snap;       // NULL until the first publish
  const AlarmEvent* events;   // drained this wake, in arrival order
  uint8_t eventCount;
  MonoMs now;
  // WP8. Zero-initialised inputs keep the WP7 behaviour.
  const SafetyCommand* commands;
  uint8_t commandCount;        // at most SAFETY_MAX_COMMANDS are handled
  SensorMask sensorDisabled;   // bit per SensorId switched off by Configuration
  // WP9. The external 5 V rail is not valid (rail_state): the relay coil has
  // no supply, so the relay output is driven OFF (unavailable-actuator
  // policy, §13) and the lamp restarts from OFF, with its dwell, when it returns.
  bool railDown;
} SafetyInputs;

typedef struct SafetyOutputs {
  bool relayOn;          // what relay 1 must be
  bool lampWantsOn;      // lamp policy decision before interlock / enable
  LampReason lampReason;
  bool interlock;
  uint16_t latchedMask;  // bit per rule index
  uint16_t activeMask;
  uint16_t staleMask;    // enabled rules whose input is not fresh and OK
                         // (a disabled sensor's rule only while latched)
  SafetyCommandResult results[SAFETY_MAX_COMMANDS];
  uint8_t resultCount;
} SafetyOutputs;

// rules may be NULL with ruleCount 0. At most SAFETY_MAX_RULES are used.
void safetyCoreInit(SafetyCore* self, const SafetyConfig* config, const AlarmRule* rules,
                    uint8_t ruleCount, MonoMs now);

void safetyCoreStep(SafetyCore* self, const SafetyInputs* in, SafetyOutputs* out);

// Explicit reset of a latched rule (WP8 command). Refused (false) while the
// rule is still active: clearing needs a valid recovery observation first.
bool safetyCoreResetLatch(SafetyCore* self, uint8_t ruleId);

// True if any enabled rule on one of this sensor's metrics is latched or
// active in the given masks. Configuration refuses to disable such a sensor:
// switching an input off must never silence an alarm (§12).
bool safetyRulesBlockSensor(const AlarmRule* rules, uint8_t ruleCount, uint16_t latchedMask,
                            uint16_t activeMask, SensorId sensor);

const char* lampReasonName(LampReason r);

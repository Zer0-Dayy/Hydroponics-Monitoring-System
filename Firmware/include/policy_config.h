// Control policy: grow-lamp thresholds and alarm rules (Stage 4, WP7).
// NOT a frozen contract: these are placeholders to be replaced by measured /
// commissioned values. Pure C (no Arduino headers).
//
// Lamp: values from the test_lamp bench (README "Lamp relay bring-up"). Set
// POLICY_LAMP_OFF_ABOVE_MV above the "night + lamp ON" reading measured at the
// final sensor position, or the lamp will switch itself off.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "domain/alarm.h"

// 1 = Safety drives the relay from the lamp decision. 0 = Safety still
// computes and reports the decision but keeps the relay de-energised.
#define POLICY_LAMP_AUTO_ENABLED 1

#define POLICY_LAMP_ON_BELOW_MV 40.0f
#define POLICY_LAMP_OFF_ABOVE_MV 120.0f
#define POLICY_LAMP_MIN_ON_MS ((uint32_t)30000)   // bench; minutes in real use
#define POLICY_LAMP_MIN_OFF_MS ((uint32_t)30000)
#define POLICY_LAMP_FAULTS_TO_OFF ((uint8_t)5)
// Light results older than this force the lamp OFF (§10 ordinary limit).
#define POLICY_LIGHT_STALE_MS CFG_STALE_ORDINARY_MS

// Analog calibration used by production acquisition (README "pH-4502C
// calibration", "SEN0244 TDS bring-up"). Nominal values: pH and TDS stay
// HEALTH_UNCALIBRATED until buffer / reference-solution fits replace them.
#define POLICY_PH_NOMINAL_MV_AT_PH7 2758.0f    // BNC shorted, 17 Oct 2026
#define POLICY_PH_NOMINAL_PH_PER_MV (-0.0057f)  // tutorial sketch slope

// On-device calibration (Calibration page) plausibility limits. A fit outside
// them is refused ("bad_fit"), never stored.
#define POLICY_PH_SLOPE_MIN_ABS 0.002f   // pH per module mV (nominal 0.0057)
#define POLICY_PH_SLOPE_MAX_ABS 0.02f
#define POLICY_TDS_K_MIN 0.5f            // K = reference ppm / nominal ppm
#define POLICY_TDS_K_MAX 2.0f
#define POLICY_LIGHT_LUX_PER_MV_MIN 0.01f
#define POLICY_LIGHT_LUX_PER_MV_MAX 100.0f
// cal_capture uses a reading at most this old (2 frames + slack).
#define POLICY_CAL_CAPTURE_MAX_AGE_MS ((uint32_t)5000)

// WP9 low-power mode (services/power_coordinator.h). 1 = losing the external
// 5 V moves sensing to the low-power worker (3.3 V sensors only) and switches
// Wi-Fi off. 0 = kill switch: report the sense level only, change nothing.
// OFF until the WP9 bench check (README "Low-power mode (WP9)"): the code
// has not run on hardware, and GPIO8 has no pull, so without the sense
// divider wired the board would boot into low power or flap. Architecture
// §19: do not activate transitions before the sense input and rail behaviour
// are verified. Set 1 for that bench check.
#define POLICY_POWER_TRANSITIONS_ENABLED 0

// Retry interval for a driver whose init() failed (§18: bounded, no loop).
#define POLICY_INIT_RETRY_MS ((uint32_t)5000)

// Alarm rules. ruleId = index. All DISABLED: no trip/clear limit has been
// commissioned (§20). Enable a rule only with measured, agreed limits.
#define POLICY_RULE_COUNT 2
static const AlarmRule kPolicyRules[POLICY_RULE_COUNT] = {
#ifdef FW_ALARM_LATENCY_TEST
    // Test build only (env:alarm_latency): trips on every valid air sample,
    // non-latching; acquisition re-arms it after each TRIP (§19 latency test).
    {.ruleId = 0, .enabled = true, .metric = METRIC_AIR_TEMPERATURE,
     .comparison = COMPARISON_ABOVE, .tripThreshold = -100.0f, .clearThreshold = -200.0f,
     .persistMs = 0, .severity = SEVERITY_CRITICAL, .latching = false,
     .actuatorId = ACTUATOR_RELAY1, .action = ACTUATOR_ACTION_DEENERGIZE},
#else
    // Example from §12: air over-temperature, latched, relay de-energised.
    {.ruleId = 0, .enabled = false, .metric = METRIC_AIR_TEMPERATURE,
     .comparison = COMPARISON_ABOVE, .tripThreshold = 40.0f, .clearThreshold = 37.0f,
     .persistMs = 0, .severity = SEVERITY_CRITICAL, .latching = true,
     .actuatorId = ACTUATOR_RELAY1, .action = ACTUATOR_ACTION_DEENERGIZE},
#endif
    // Water temperature high, warning only.
    {.ruleId = 1, .enabled = false, .metric = METRIC_WATER_TEMPERATURE,
     .comparison = COMPARISON_ABOVE, .tripThreshold = 30.0f, .clearThreshold = 28.0f,
     .persistMs = 10000, .severity = SEVERITY_WARNING, .latching = false,
     .actuatorId = ACTUATOR_NONE, .action = ACTUATOR_ACTION_NONE},
};

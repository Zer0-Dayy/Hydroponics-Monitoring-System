// Host unit tests for src/domain (pio test -e native). Architecture §17 stage 4.
// Domain code must stay free of Arduino/FreeRTOS so these run on a PC.
#include <unity.h>

#include "app_config.h"
#include "board_profile.h"
#include <math.h>
#include <string.h>

#include "domain/alarm.h"
#include "domain/alarm_sticky.h"
#include "domain/calibration.h"
#include "domain/ds18b20_codec.h"
#include "domain/lamp_control.h"
#include "domain/reading.h"
#include "domain/safety_core.h"
#include "domain/schedule.h"
#include "domain/sensor_set.h"
#include "domain/sgp30_codec.h"
#include "domain/tds_codec.h"
#include "domain/ultrasonic.h"

void setUp(void) {}
void tearDown(void) {}

static void test_reset_reading_is_starting_and_not_actionable(void) {
  Reading r;
  readingReset(&r, SENSOR_BME280, METRIC_AIR_TEMPERATURE, UNIT_DEG_C);
  TEST_ASSERT_TRUE(r.health == HEALTH_STARTING);
  TEST_ASSERT_TRUE(r.sensor == SENSOR_BME280);
  TEST_ASSERT_FALSE(readingIsActionable(&r));
}

static void test_ok_reading_becomes_stale_after_limit(void) {
  Reading r;
  readingReset(&r, SENSOR_DS18B20, METRIC_WATER_TEMPERATURE, UNIT_DEG_C);
  r.health = HEALTH_OK;
  r.intervalEndMs = 1000;
  TEST_ASSERT_TRUE(readingHealthAt(&r, 2000, 1000) == HEALTH_OK);
  TEST_ASSERT_TRUE(readingHealthAt(&r, 2001, 1000) == HEALTH_STALE);
}

static void test_stale_reading_is_not_actionable(void) {
  Reading r;
  readingReset(&r, SENSOR_PH, METRIC_PH, UNIT_PH);
  r.health = HEALTH_OK;
  r.intervalEndMs = 1000;
  TEST_ASSERT_TRUE(readingIsActionableAt(&r, 4000, 3000));   // exactly at the limit
  TEST_ASSERT_FALSE(readingIsActionableAt(&r, 4001, 3000));  // stale
  TEST_ASSERT_TRUE(readingIsActionable(&r));                 // health-only check is unchanged
  r.health = HEALTH_UNCALIBRATED;
  TEST_ASSERT_FALSE(readingIsActionableAt(&r, 1000, 3000));  // fresh but not OK
}

static void test_fault_is_not_masked_by_staleness(void) {
  Reading r;
  readingReset(&r, SENSOR_PH, METRIC_PH, UNIT_PH);
  r.health = HEALTH_UNPOWERED;
  TEST_ASSERT_TRUE(readingHealthAt(&r, 100000, 1000) == HEALTH_UNPOWERED);
}

static void test_divider_recovery_uses_nominal_ratios(void) {
  // Power sense: 220/440 => x660/440. 3333 mV at GPIO8 -> 5000 mV external feed.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 4999.5f,
                           dividerRecoverMv(3333.0f, BOARD_POWER_SENSE_DIVIDER_TOP_OHM,
                                            BOARD_POWER_SENSE_DIVIDER_BOTTOM_OHM));
  // pH: 10k/15k => x25/15. 3000 mV at the pin -> 5000 mV at the module.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 5000.0f,
                           dividerRecoverMv(3000.0f, BOARD_PH_DIVIDER_TOP_OHM,
                                            BOARD_PH_DIVIDER_BOTTOM_OHM));
}

static void test_two_point_fit_maps_buffers(void) {
  // Module mV in pH 6.86 and 4.01 buffers, about -5.7 pH/V (tutorial).
  LinearCalibration c;
  TEST_ASSERT_TRUE(linearFromTwoPoints(2500.0f, 6.86f, 3000.0f, 4.01f, 1, &c));
  TEST_ASSERT_TRUE(c.valid);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.0057f, c.slope);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.86f, linearApply(&c, 2500.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.01f, linearApply(&c, 3000.0f));
  TEST_ASSERT_FALSE(linearFromTwoPoints(2500.0f, 6.86f, 2500.0f, 4.01f, 2, &c));
  TEST_ASSERT_FALSE(c.valid);
}

static void test_trimmed_mean_rejects_spikes(void) {
  uint32_t s[10] = {1500, 4000, 1502, 1498, 0, 1501, 1499, 1500, 3100, 1500};
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1500.333f, trimmedMeanU32(s, 10, 2));  // 1499..1502
  TEST_ASSERT_TRUE(s[0] == 0 && s[9] == 4000);  // sorted in place
  uint32_t few[4] = {1, 2, 3, 4};
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, trimmedMeanU32(few, 4, 2));
}

static void test_snapshot_reset_indexes_by_metric(void) {
  Snapshot s;
  snapshotReset(&s);
  for (uint8_t i = 0; i < METRIC_COUNT; ++i) {
    TEST_ASSERT_TRUE(s.readings[i].metric == (MetricId)i);
    TEST_ASSERT_TRUE(s.readings[i].health == HEALTH_STARTING);
  }
}

static void test_production_ready_requires_power_sense(void) {
  BoardValidation v = boardProfileValidate();
  TEST_ASSERT_EQUAL(v.powerSenseAssigned && BOARD_POWER_SENSE_VERIFIED, v.productionReady);
}

static void test_snapshot_reset_sets_identity(void) {
  Snapshot s;
  snapshotReset(&s);
  TEST_ASSERT_EQUAL_UINT8(SENSOR_BME280, s.readings[METRIC_AIR_HUMIDITY].sensor);
  TEST_ASSERT_EQUAL_UINT8(UNIT_PERCENT_RH, s.readings[METRIC_AIR_HUMIDITY].unit);
  TEST_ASSERT_EQUAL_UINT8(SENSOR_DS18B20, s.readings[METRIC_WATER_TEMPERATURE].sensor);
  TEST_ASSERT_EQUAL_UINT8(SENSOR_LIGHT, s.readings[METRIC_LIGHT_LEVEL].sensor);
  TEST_ASSERT_EQUAL_UINT8(SENSOR_SGP30, s.readings[METRIC_CO2EQ].sensor);
  TEST_ASSERT_EQUAL_UINT8(UNIT_PPB, s.readings[METRIC_TVOC].unit);
  for (uint8_t i = 0; i < METRIC_COUNT; ++i) {
    TEST_ASSERT_TRUE(s.readings[i].sensor < SENSOR_COUNT);
  }
}

static void test_alarm_rule_ignores_stale_sample(void) {
  AlarmRule rule = {0};
  rule.metric = METRIC_AIR_TEMPERATURE;
  AlarmRuleState state = {0};
  Reading r;
  readingReset(&r, SENSOR_BME280, METRIC_AIR_TEMPERATURE, UNIT_DEG_C);
  r.health = HEALTH_OK;
  r.value = 50.0f;
  r.intervalEndMs = 1000;
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NOT_ELIGIBLE, evaluateRule(&rule, &r, 2001, 1000, &state));
  TEST_ASSERT_TRUE(evaluateRule(&rule, &r, 1500, 1000, &state) != RULE_DECISION_NOT_ELIGIBLE);
}

static void test_sgp30_crc_matches_datasheet_vector(void) {
  const uint8_t beef[2] = {0xBE, 0xEF};
  TEST_ASSERT_EQUAL_HEX8(0x92, sgp30Crc8(beef, 2));
}

static void test_sgp30_words_round_trip_and_reject_bad_crc(void) {
  uint8_t buf[6];
  sgp30PackWord(0x01A3, &buf[0]);  // CO2eq 419 ppm
  sgp30PackWord(0x0007, &buf[3]);  // TVOC 7 ppb
  uint16_t w[2] = {0, 0};
  TEST_ASSERT_TRUE(sgp30UnpackWords(buf, 2, w));
  TEST_ASSERT_EQUAL_HEX16(0x01A3, w[0]);
  TEST_ASSERT_EQUAL_HEX16(0x0007, w[1]);
  buf[4] ^= 0x01;  // one flipped bit in the second word
  TEST_ASSERT_FALSE(sgp30UnpackWords(buf, 2, w));
  const uint8_t floating[3] = {0xFF, 0xFF, 0xFF};  // released bus
  TEST_ASSERT_FALSE(sgp30UnpackWords(floating, 1, w));
}

static void test_sgp30_humidity_fixed_point(void) {
  TEST_ASSERT_EQUAL_HEX16(0x0F80, sgp30GramsToFixed8p8(15.5f));  // datasheet example
  TEST_ASSERT_EQUAL_HEX16(0x0000, sgp30GramsToFixed8p8(0.0f));
  TEST_ASSERT_EQUAL_HEX16(0x0001, sgp30GramsToFixed8p8(0.001f));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, sgp30GramsToFixed8p8(300.0f));
  // The datasheet formula gives 11.48 g/m3 at 25 degC / 50 %RH (its text rounds
  // this example to 11.8).
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.48f, sgp30AbsoluteHumidity(25.0f, 50.0f));
  TEST_ASSERT_TRUE(sgp30AbsoluteHumidity(25.0f, 120.0f) < 0.0f);
}

static void test_ultrasonic_echo_width_to_mm(void) {
  // 58.3 us per cm at 343 m/s (HC-SR04 guide, equation 3).
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 1000.0f,
                           ultrasonicEchoUsToMm(5831, ULTRASONIC_NOMINAL_SPEED_MPS));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f,
                           ultrasonicEchoUsToMm(117, ULTRASONIC_NOMINAL_SPEED_MPS));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, ultrasonicEchoUsToMm(0, ULTRASONIC_NOMINAL_SPEED_MPS));
}

static void test_ultrasonic_range_is_2_to_400_cm(void) {
  TEST_ASSERT_TRUE(ultrasonicInRangeMm(20.0f));
  TEST_ASSERT_TRUE(ultrasonicInRangeMm(4000.0f));
  TEST_ASSERT_FALSE(ultrasonicInRangeMm(19.9f));
  TEST_ASSERT_FALSE(ultrasonicInRangeMm(4000.1f));
  TEST_ASSERT_FALSE(ultrasonicInRangeMm(0.0f));  // never a valid distance
}

static void test_tds_datasheet_curve(void) {
  // (133.42 - 255.86 + 857.39) * 0.5 at 1.0 V, 25 degC (SEN0244 sample code).
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, tdsCompensationCoefficient(25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 367.475f, tdsPpmFromModuleMv(1000.0f, 1.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, tdsPpmFromModuleMv(0.0f, 1.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, tdsPpmFromModuleMv(1000.0f, 0.0f));
  // 2.3 V (top of the output range) is above the rated 1000 ppm.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1120.9f, tdsPpmFromModuleMv(2300.0f, 1.0f));
  TEST_ASSERT_TRUE(tdsPpmFromModuleMv(2300.0f, 1.0f) > TDS_FULL_SCALE_PPM);
}

static void test_tds_temperature_compensation(void) {
  // The conductivity (ppm) is compensated, not the voltage (GravityTDS).
  const float coefficient = tdsCompensationCoefficient(35.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.2f, coefficient);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 367.475f / 1.2f, tdsPpmFromModuleMv(1000.0f, coefficient));
  // Measured 24 Oct 2026, same tap water cold and warm: within ~2 % at 25 degC.
  const float cold = tdsPpmFromModuleMv(1234.0f, tdsCompensationCoefficient(11.44f));
  const float warm = tdsPpmFromModuleMv(1768.0f, tdsCompensationCoefficient(33.62f));
  TEST_ASSERT_FLOAT_WITHIN(0.03f * cold, cold, warm);
}

// Scratchpad with the given temperature register and 12-bit config, valid CRC.
static void makeScratchpad(int16_t raw, uint8_t sp[DS18B20_SCRATCHPAD_LEN]) {
  const uint8_t base[8] = {(uint8_t)(raw & 0xFF), (uint8_t)((uint16_t)raw >> 8), 0x4B, 0x46,
                           DS18B20_CONFIG_12BIT, 0xFF, 0x0C, 0x10};
  for (uint8_t i = 0; i < 8; ++i) sp[i] = base[i];
  sp[8] = oneWireCrc8(sp, 8);
}

static void test_onewire_crc_matches_maxim_vector(void) {
  // Maxim application note 27: ROM 02 1C B8 01 00 00 00 has CRC A2.
  const uint8_t rom[8] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA2};
  TEST_ASSERT_EQUAL_HEX8(0xA2, oneWireCrc8(rom, 7));
  TEST_ASSERT_EQUAL_HEX8(0x00, oneWireCrc8(rom, 8));  // data + CRC -> 0
  TEST_ASSERT_FALSE(ds18b20RomValid(rom));  // family 02h, not a DS18B20
  uint8_t ds[8] = {0x28, 0xFF, 0x64, 0x1E, 0x0F, 0x4A, 0x3C, 0x00};
  ds[7] = oneWireCrc8(ds, 7);
  TEST_ASSERT_TRUE(ds18b20RomValid(ds));
  ds[3] ^= 0x01;
  TEST_ASSERT_FALSE(ds18b20RomValid(ds));
  const uint8_t zero[8] = {0};  // bus stuck low: CRC 0 matches, family does not
  TEST_ASSERT_FALSE(ds18b20RomValid(zero));
}

static void test_ds18b20_temperature_table(void) {
  // Datasheet Table 1.
  uint8_t sp[DS18B20_SCRATCHPAD_LEN];
  makeScratchpad(0x0191, sp);
  TEST_ASSERT_TRUE(ds18b20ScratchpadValid(sp));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 25.0625f, ds18b20RawToDegC(ds18b20RawTemperature(sp)));
  makeScratchpad((int16_t)0xFF5E, sp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -10.125f, ds18b20RawToDegC(ds18b20RawTemperature(sp)));
  makeScratchpad((int16_t)0xFC90, sp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -55.0f, ds18b20RawToDegC(ds18b20RawTemperature(sp)));
  makeScratchpad(0x07D0, sp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 125.0f, ds18b20RawToDegC(ds18b20RawTemperature(sp)));
  makeScratchpad(DS18B20_POWER_ON_RAW, sp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 85.0f, ds18b20RawToDegC(ds18b20RawTemperature(sp)));
}

static void test_ds18b20_scratchpad_checks(void) {
  uint8_t sp[DS18B20_SCRATCHPAD_LEN];
  makeScratchpad(0x0191, sp);
  sp[0] ^= 0x01;  // one bit flipped on the wire
  TEST_ASSERT_FALSE(ds18b20ScratchpadValid(sp));
  const uint8_t zero[DS18B20_SCRATCHPAD_LEN] = {0};  // CRC 0 matches, config does not
  TEST_ASSERT_FALSE(ds18b20ScratchpadValid(zero));
  uint8_t ff[DS18B20_SCRATCHPAD_LEN];  // floating / no device
  for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; ++i) ff[i] = 0xFF;
  TEST_ASSERT_FALSE(ds18b20ScratchpadValid(ff));
  TEST_ASSERT_TRUE(ds18b20Is12Bit(0x7F));
  TEST_ASSERT_FALSE(ds18b20Is12Bit(0x5F));  // 11-bit
  TEST_ASSERT_FALSE(ds18b20Is12Bit(0x1F));  // 9-bit
}

static const LampControlConfig kLampTest = {40.0f, 120.0f, 30000, 30000, 3};

static Reading lampLight(float mv, Health health) {
  Reading r;
  readingReset(&r, SENSOR_LIGHT, METRIC_LIGHT_LEVEL, UNIT_MILLIVOLT);
  r.health = health;
  r.value = mv;
  return r;
}

static void test_lamp_hysteresis_band(void) {
  LampControl c;
  lampControlInit(&c, &kLampTest, 0);
  Reading r = lampLight(80.0f, HEALTH_UNCALIBRATED);  // inside the band: stays OFF
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &r, 60000));
  r.value = 39.0f;
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 61000));
  r.value = 119.0f;  // inside the band: stays ON
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 200000));
  r.value = 121.0f;
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &r, 201000));
}

// After an on-device lux fit the value is lux; the thresholds stay pin mV (raw).
static void test_lamp_uses_pin_mv_after_lux_fit(void) {
  LampControl c;
  lampControlInit(&c, &kLampTest, 0);
  Reading r = lampLight(10.0f, HEALTH_OK);  // 10 mV at the pin: dark
  r.unit = UNIT_LUX;
  r.value = 500.0f;  // would be "bright" if read as mV
  r.hasRaw = true;
  r.raw = 10;
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 60000));
}

static void test_lamp_dwell_holds_changes(void) {
  LampControl c;
  lampControlInit(&c, &kLampTest, 0);
  Reading r = lampLight(10.0f, HEALTH_OK);
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &r, 29999));  // boot counts as OFF
  TEST_ASSERT_TRUE(c.waiting);
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 30000));
  TEST_ASSERT_FALSE(c.waiting);
  r.value = 500.0f;
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 59999));
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &r, 60000));
}

static void test_lamp_faults_hold_then_force_off(void) {
  LampControl c;
  lampControlInit(&c, &kLampTest, 0);
  Reading r = lampLight(10.0f, HEALTH_OK);
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 30000));
  Reading bad = lampLight(0.0f, HEALTH_FAULT);
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &bad, 31000));
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &bad, 32000));
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 33000));  // a good reading resets the count
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &bad, 34000));
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &bad, 35000));
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &bad, 36000));  // 3rd in a row, no dwell
}

static void test_lamp_saturated_counts_as_bright(void) {
  LampControl c;
  lampControlInit(&c, &kLampTest, 0);
  Reading r = lampLight(10.0f, HEALTH_OK);
  TEST_ASSERT_TRUE(lampControlUpdate(&c, &r, 30000));
  r.value = 20.0f;  // a clipped value is only a lower bound
  r.qualityFlags = kQualitySaturated;
  TEST_ASSERT_FALSE(lampControlUpdate(&c, &r, 60000));
}

// ---- alarm rules (§12) --------------------------------------------------------

static const AlarmRule kHotRule = {.ruleId = 0, .enabled = true, .metric = METRIC_AIR_TEMPERATURE,
                                   .comparison = COMPARISON_ABOVE, .tripThreshold = 40.0f,
                                   .clearThreshold = 37.0f, .persistMs = 0,
                                   .severity = SEVERITY_CRITICAL, .latching = true,
                                   .actuatorId = ACTUATOR_RELAY1,
                                   .action = ACTUATOR_ACTION_DEENERGIZE};

static Reading airTemp(float degC, MonoMs endMs) {
  Reading r;
  readingReset(&r, SENSOR_BME280, METRIC_AIR_TEMPERATURE, UNIT_DEG_C);
  r.health = HEALTH_OK;
  r.value = degC;
  r.intervalEndMs = endMs;
  return r;
}

static RuleDecision evalAt(const AlarmRule* rule, float v, MonoMs t, AlarmRuleState* st) {
  Reading r = airTemp(v, t);
  return evaluateRule(rule, &r, t, 1000, st);
}

static void test_alarm_trips_on_one_sample_and_clears_with_hysteresis(void) {
  AlarmRuleState st = {0};
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&kHotRule, 39.9f, 250, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_TRIP, evalAt(&kHotRule, 40.0f, 500, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&kHotRule, 45.0f, 750, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&kHotRule, 38.0f, 1000, &st));  // in band
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_CLEAR, evalAt(&kHotRule, 37.0f, 1250, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&kHotRule, 39.0f, 1500, &st));  // no chatter
}

static void test_alarm_persistence_needs_sustained_valid_samples(void) {
  AlarmRule rule = kHotRule;
  rule.persistMs = 1000;
  AlarmRuleState st = {0};
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&rule, 41.0f, 1000, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&rule, 41.0f, 1999, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_TRIP, evalAt(&rule, 41.0f, 2000, &st));

  AlarmRuleState st2 = {0};
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&rule, 41.0f, 1000, &st2));
  Reading bad = airTemp(41.0f, 1500);
  bad.health = HEALTH_FAULT;  // a gap restarts the window
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NOT_ELIGIBLE, evaluateRule(&rule, &bad, 1500, 1000, &st2));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&rule, 41.0f, 2000, &st2));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_TRIP, evalAt(&rule, 41.0f, 3000, &st2));
}

static void test_alarm_below_rule_mirrors_above(void) {
  AlarmRule rule = kHotRule;
  rule.comparison = COMPARISON_BELOW;
  rule.tripThreshold = 5.0f;
  rule.clearThreshold = 8.0f;
  AlarmRuleState st = {0};
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_TRIP, evalAt(&rule, 4.0f, 100, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_NO_CHANGE, evalAt(&rule, 7.0f, 200, &st));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_CLEAR, evalAt(&rule, 8.5f, 300, &st));
}

static void test_alarm_stale_limits_per_metric(void) {
  TEST_ASSERT_EQUAL_UINT32(CFG_STALE_BME280_MS, alarmStaleLimitMs(METRIC_AIR_HUMIDITY));
  TEST_ASSERT_EQUAL_UINT32(CFG_STALE_ORDINARY_MS, alarmStaleLimitMs(METRIC_LIGHT_LEVEL));
  TEST_ASSERT_EQUAL_UINT32(CFG_STALE_ORDINARY_MS, alarmStaleLimitMs(METRIC_TVOC));
}

// ---- schedule (§9) ----------------------------------------------------------

static void test_schedule_bme280_runs_four_times_per_frame(void) {
  ScheduleJob jobs[2];
  scheduleJobInit(&jobs[0], CFG_FRAME_PERIOD_MS, 20, 0);
  scheduleJobInit(&jobs[1], CFG_BME280_PERIOD_MS, CFG_BME280_BUDGET_MS, 0);
  uint32_t frameRuns = 0, bmeRuns = 0;
  MonoMs now = 0;
  while (now < 1000) {
    if (scheduleJobDue(&jobs[0], now)) { scheduleJobMarkRun(&jobs[0], now); ++frameRuns; }
    if (scheduleJobDue(&jobs[1], now)) { scheduleJobMarkRun(&jobs[1], now); ++bmeRuns; }
    now = scheduleEarliestDue(jobs, 2);
  }
  TEST_ASSERT_EQUAL_UINT32(1, frameRuns);
  TEST_ASSERT_EQUAL_UINT32(4, bmeRuns);
  TEST_ASSERT_EQUAL_UINT32(0, jobs[1].late);
  TEST_ASSERT_TRUE(jobs[1].nextDueMs == 1000);
}

static void test_schedule_skips_missed_slots_without_burst(void) {
  ScheduleJob j;
  scheduleJobInit(&j, 250, 12, 500);
  scheduleJobMarkRun(&j, 530);  // 30 ms late: keeps the 250 ms grid
  TEST_ASSERT_TRUE(j.nextDueMs == 750);
  TEST_ASSERT_EQUAL_UINT32(1, j.late);
  TEST_ASSERT_EQUAL_UINT32(0, j.skipped);
  TEST_ASSERT_EQUAL_UINT32(30, j.maxLateMs);

  scheduleJobMarkRun(&j, 3760);  // stalled ~3 s: one run, the rest skipped
  TEST_ASSERT_TRUE(j.nextDueMs == 4000);
  TEST_ASSERT_EQUAL_UINT32(12, j.skipped);
  TEST_ASSERT_FALSE(scheduleJobDue(&j, 3761));
  TEST_ASSERT_EQUAL_UINT32(2, j.runs);
}

static void test_schedule_jitter_within_tolerance_is_not_late(void) {
  ScheduleJob j;
  scheduleJobInit(&j, 1000, 10, 1000);
  scheduleJobMarkRun(&j, 1000 + SCHEDULE_LATE_TOLERANCE_MS);
  TEST_ASSERT_EQUAL_UINT32(0, j.late);
  TEST_ASSERT_EQUAL_UINT32(SCHEDULE_LATE_TOLERANCE_MS, j.lastLateMs);
  TEST_ASSERT_TRUE(j.nextDueMs == 2000);
}

static void test_schedule_defers_step_crossing_bme280_slot(void) {
  TEST_ASSERT_FALSE(scheduleFitsBefore(230, 50, 250));  // DS18B20 init ~50 ms
  TEST_ASSERT_TRUE(scheduleFitsBefore(230, 19, 250));
  TEST_ASSERT_TRUE(scheduleFitsBefore(262, 50, 500));
}

// ---- safety core (§12) ------------------------------------------------------

static const SafetyConfig kSafetyTest = {true, {40.0f, 120.0f, 30000, 30000, 3}, 3000};

static void setLight(Snapshot* s, float mv, Health h, MonoMs endMs) {
  Reading* r = &s->readings[METRIC_LIGHT_LEVEL];
  r->health = h;
  r->value = mv;
  r->intervalEndMs = endMs;
  ++r->sequence;
}

static SafetyOutputs step(SafetyCore* c, const Snapshot* s, MonoMs now, const AlarmEvent* ev,
                          uint8_t n) {
  SafetyInputs in = {s, ev, n, now};
  SafetyOutputs out;
  safetyCoreStep(c, &in, &out);
  return out;
}

// Dark from t=1 s, one light result and one 25 degC air temperature per
// second (the rules' input stays fresh); returns the core at t=31 s with the
// lamp ON (boot dwell 30 s).
static void lampOnAt31s(SafetyCore* c, Snapshot* s, const AlarmRule* rules, uint8_t n) {
  safetyCoreInit(c, &kSafetyTest, rules, n, 0);
  snapshotReset(s);
  for (MonoMs t = 1000; t <= 31000; t += 1000) {
    setLight(s, 10.0f, HEALTH_UNCALIBRATED, t);
    s->readings[METRIC_AIR_TEMPERATURE] = airTemp(25.0f, t);
    step(c, s, t, NULL, 0);
  }
}

static void test_safety_boot_off_until_data_and_dwell(void) {
  SafetyCore c;
  safetyCoreInit(&c, &kSafetyTest, NULL, 0, 0);
  SafetyOutputs o = step(&c, NULL, 100, NULL, 0);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_NO_DATA, o.lampReason);

  Snapshot s;
  snapshotReset(&s);
  o = step(&c, &s, 200, NULL, 0);  // published, but no light result yet
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_NO_DATA, o.lampReason);

  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 1000);  // dark, boot dwell running
  o = step(&c, &s, 1000, NULL, 0);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_DWELL, o.lampReason);
}

// WP9: the relay coil is on the external 5 V. Rail down = relay OFF, and the
// lamp restarts from OFF (with its dwell) once the rail is back.
static void test_safety_rail_down_forces_relay_off(void) {
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, NULL, 0);
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 32000);
  SafetyInputs in = {&s, NULL, 0, 32000, NULL, 0, 0, true};
  SafetyOutputs o;
  safetyCoreStep(&c, &in, &o);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL(LAMP_REASON_UNPOWERED, o.lampReason);
  TEST_ASSERT_EQUAL_STRING("no 5 V", lampReasonName(o.lampReason));
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 33000);
  o = step(&c, &s, 33000, NULL, 0);  // rail back: still dark, but the OFF dwell holds
  TEST_ASSERT_FALSE(o.relayOn);
}

static void test_safety_dark_turns_lamp_on_after_dwell(void) {
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, NULL, 0);
  SafetyOutputs o = step(&c, &s, 31100, NULL, 0);
  TEST_ASSERT_TRUE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_LEVEL, o.lampReason);
}

static void test_safety_stale_light_forces_off_without_new_sample(void) {  // audit B1
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, NULL, 0);
  // Acquisition stalls: the snapshot keeps the reading that ended at 31 s.
  TEST_ASSERT_TRUE(step(&c, &s, 33900, NULL, 0).relayOn);
  TEST_ASSERT_TRUE(step(&c, &s, 34000, NULL, 0).relayOn);  // exactly 3 s: still fresh
  SafetyOutputs o = step(&c, &s, 34100, NULL, 0);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_STALE, o.lampReason);
  // Data resumes, still dark: the OFF dwell started at the forced OFF.
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 35000);
  TEST_ASSERT_FALSE(step(&c, &s, 35000, NULL, 0).relayOn);
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 64100);
  TEST_ASSERT_TRUE(step(&c, &s, 64100, NULL, 0).relayOn);
}

static void test_safety_faults_count_per_result_not_per_wake(void) {
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, NULL, 0);
  setLight(&s, 0.0f, HEALTH_FAULT, 32000);
  for (MonoMs t = 32000; t < 33000; t += 100) {  // ten wakes, one FAULT result
    TEST_ASSERT_TRUE(step(&c, &s, t, NULL, 0).relayOn);
  }
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_FAULT, step(&c, &s, 33000, NULL, 0).lampReason);
  setLight(&s, 0.0f, HEALTH_FAULT, 33000);
  TEST_ASSERT_TRUE(step(&c, &s, 33000, NULL, 0).relayOn);
  setLight(&s, 0.0f, HEALTH_FAULT, 34000);  // third in a row (faultsToOff = 3)
  TEST_ASSERT_FALSE(step(&c, &s, 34000, NULL, 0).relayOn);
}

static AlarmEvent hotEvent(RuleDecision d, float v, MonoMs t) {
  AlarmEvent e;
  e.ruleId = 0;
  e.decision = d;
  e.sample = airTemp(v, t);
  return e;
}

static void test_safety_latched_interlock_beats_lamp_until_reset(void) {
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, &kHotRule, 1);
  const AlarmEvent trip = hotEvent(RULE_DECISION_TRIP, 41.0f, 31200);
  SafetyOutputs o = step(&c, &s, 31200, &trip, 1);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_TRUE(o.interlock);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_INTERLOCK, o.lampReason);
  TEST_ASSERT_EQUAL_HEX16(0x0001, o.latchedMask);

  TEST_ASSERT_FALSE(safetyCoreResetLatch(&c, 0));  // still active: refused
  const AlarmEvent clear = hotEvent(RULE_DECISION_CLEAR, 36.0f, 40000);
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 40000);
  o = step(&c, &s, 40000, &clear, 1);
  TEST_ASSERT_FALSE(o.relayOn);  // cleared but latched
  TEST_ASSERT_EQUAL_HEX16(0x0001, o.latchedMask);
  TEST_ASSERT_EQUAL_HEX16(0x0000, o.activeMask);

  TEST_ASSERT_TRUE(safetyCoreResetLatch(&c, 0));
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 41000);
  s.readings[METRIC_AIR_TEMPERATURE] = airTemp(36.0f, 41000);
  TEST_ASSERT_FALSE(step(&c, &s, 41000, NULL, 0).relayOn);  // OFF dwell from the interlock
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 61300);
  s.readings[METRIC_AIR_TEMPERATURE] = airTemp(36.0f, 61300);
  TEST_ASSERT_TRUE(step(&c, &s, 61300, NULL, 0).relayOn);
}

// Audit P2: an enabled relay-interlock rule without fresh input cannot see a
// fault, so the relay is held OFF (not an interlock: nothing tripped).
static void test_safety_missing_interlock_input_holds_relay_off(void) {
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, &kHotRule, 1);
  SafetyOutputs o = step(&c, &s, 31000, NULL, 0);
  TEST_ASSERT_TRUE(o.relayOn);
  setLight(&s, 10.0f, HEALTH_UNCALIBRATED, 32000);
  o = step(&c, &s, 32100, NULL, 0);  // temperature 1.1 s old, light fresh and dark
  TEST_ASSERT_EQUAL_HEX16(0x0001, o.staleMask);
  TEST_ASSERT_FALSE(o.interlock);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_INPUT_MISSING, o.lampReason);
  TEST_ASSERT_EQUAL_STRING("input missing", lampReasonName(o.lampReason));
}

static AlarmEvent stickyTrip(float v) {
  AlarmEvent e;
  memset(&e, 0, sizeof(e));
  e.ruleId = 0;
  e.decision = RULE_DECISION_TRIP;
  e.sample = airTemp(v, 1000);
  return e;
}

// Audit P1: the send fails, Safety drains before the "lost" mark: the TRIP
// must still come out of the sticky slot on the next pass.
static void test_sticky_trip_survives_drain_during_failed_send(void) {
  StickySlot slot;
  memset(&slot, 0, sizeof(slot));
  const bool sawTrip[1] = {false};
  AlarmEvent out[2];
  const AlarmEvent trip = stickyTrip(45.0f);
  stickyBegin(&slot, &kHotRule, &trip);
  TEST_ASSERT_EQUAL_UINT8(0, stickyCollect(&slot, 1, sawTrip, out));  // in flight: untouched
  stickyEnd(&slot, false);
  TEST_ASSERT_EQUAL_UINT8(1, stickyCollect(&slot, 1, sawTrip, out));
  TEST_ASSERT_EQUAL_UINT8(RULE_DECISION_TRIP, out[0].decision);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 45.0f, out[0].sample.value);
  TEST_ASSERT_EQUAL_UINT8(0, stickyCollect(&slot, 1, sawTrip, out));  // delivered once
}

static void test_sticky_queued_record_is_not_duplicated(void) {
  StickySlot slot;
  memset(&slot, 0, sizeof(slot));
  AlarmEvent out[2];
  const AlarmEvent trip = stickyTrip(41.0f);
  stickyBegin(&slot, &kHotRule, &trip);
  stickyEnd(&slot, true);
  const bool sawTrip[1] = {true};
  TEST_ASSERT_EQUAL_UINT8(0, stickyCollect(&slot, 1, sawTrip, out));
  TEST_ASSERT_FALSE(slot.tripSinceDrain);
}

// Audit P2: consumers re-check 5 V readings against the live rail epoch.
static void test_snapshot_gate_rail_marks_old_epoch_unpowered(void) {
  Snapshot s;
  snapshotReset(&s);
  s.readings[METRIC_PH].health = HEALTH_UNCALIBRATED;
  s.readings[METRIC_PH].powerEpoch = 4;
  s.readings[METRIC_TDS].health = HEALTH_OK;
  s.readings[METRIC_TDS].powerEpoch = 5;
  s.readings[METRIC_AIR_TEMPERATURE].health = HEALTH_OK;  // 3.3 V: never gated
  s.readings[METRIC_AIR_TEMPERATURE].powerEpoch = 1;
  snapshotGateRail(&s, true, 5);
  TEST_ASSERT_EQUAL_UINT8(HEALTH_UNPOWERED, s.readings[METRIC_PH].health);
  TEST_ASSERT_EQUAL_UINT8(HEALTH_OK, s.readings[METRIC_TDS].health);
  TEST_ASSERT_EQUAL_UINT8(HEALTH_OK, s.readings[METRIC_AIR_TEMPERATURE].health);
  TEST_ASSERT_EQUAL_UINT8(HEALTH_STARTING, s.readings[METRIC_WATER_DISTANCE].health);
  snapshotGateRail(&s, false, 5);  // same epoch, rail invalid now
  TEST_ASSERT_EQUAL_UINT8(HEALTH_UNPOWERED, s.readings[METRIC_TDS].health);
}

static void test_fits_reject_non_finite_values(void) {
  LinearCalibration c;
  TEST_ASSERT_FALSE(linearFromTwoPoints(NAN, 7.0f, 2000.0f, 4.0f, 1, &c));
  TEST_ASSERT_FALSE(c.valid);
  TEST_ASSERT_FALSE(linearFromTwoPoints(1000.0f, INFINITY, 2000.0f, 4.0f, 1, &c));
  TEST_ASSERT_FALSE(linearGainFromPoint(INFINITY, 1.0f, 0.0f, 1e30f, 1, &c));
  TEST_ASSERT_FALSE(phFitFromBuffers(NAN, 6.86f, 2000.0f, 4.01f, 0.001f, 0.02f, 1, &c));
  c.slope = NAN;
  c.offset = 0.0f;
  TEST_ASSERT_FALSE(linearCalibrationFinite(&c));
}

static void test_safety_trip_and_clear_in_one_batch_still_deenergises(void) {
  AlarmRule rule = kHotRule;
  rule.latching = false;
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, &rule, 1);
  const AlarmEvent batch[2] = {hotEvent(RULE_DECISION_TRIP, 50.0f, 31100),
                               hotEvent(RULE_DECISION_CLEAR, 30.0f, 31150)};
  SafetyOutputs o = step(&c, &s, 31200, batch, 2);
  TEST_ASSERT_FALSE(o.relayOn);  // the unhandled TRIP is not erased by the CLEAR
  TEST_ASSERT_TRUE(o.interlock);
  TEST_ASSERT_EQUAL_HEX16(0x0000, o.activeMask);
  TEST_ASSERT_EQUAL_UINT32(1, c.rule[0].trips);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 50.0f, c.rule[0].worst);
}

static void test_safety_disabled_rule_and_auto_switch(void) {
  AlarmRule rule = kHotRule;
  rule.enabled = false;
  SafetyCore c;
  Snapshot s;
  lampOnAt31s(&c, &s, &rule, 1);
  const AlarmEvent trip = hotEvent(RULE_DECISION_TRIP, 41.0f, 31200);
  TEST_ASSERT_TRUE(step(&c, &s, 31200, &trip, 1).relayOn);  // disabled: no interlock

  SafetyConfig off = kSafetyTest;
  off.lampAutoEnabled = false;
  safetyCoreInit(&c, &off, NULL, 0, 0);
  snapshotReset(&s);
  SafetyOutputs o = {0};
  for (MonoMs t = 1000; t <= 31000; t += 1000) {
    setLight(&s, 10.0f, HEALTH_UNCALIBRATED, t);
    o = step(&c, &s, t, NULL, 0);
  }
  TEST_ASSERT_TRUE(o.lampWantsOn);
  TEST_ASSERT_FALSE(o.relayOn);
  TEST_ASSERT_EQUAL_UINT8(LAMP_REASON_DISABLED, o.lampReason);
}

static void test_safety_reports_stale_rule_inputs(void) {
  SafetyCore c;
  Snapshot s;
  safetyCoreInit(&c, &kSafetyTest, &kHotRule, 1, 0);
  snapshotReset(&s);
  TEST_ASSERT_EQUAL_HEX16(0x0001, step(&c, &s, 100, NULL, 0).staleMask);  // never measured
  s.readings[METRIC_AIR_TEMPERATURE] = airTemp(25.0f, 1000);
  TEST_ASSERT_EQUAL_HEX16(0x0000, step(&c, &s, 1500, NULL, 0).staleMask);
  TEST_ASSERT_EQUAL_HEX16(0x0001, step(&c, &s, 2100, NULL, 0).staleMask);  // > 1 s
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_reset_reading_is_starting_and_not_actionable);
  RUN_TEST(test_ok_reading_becomes_stale_after_limit);
  RUN_TEST(test_stale_reading_is_not_actionable);
  RUN_TEST(test_fault_is_not_masked_by_staleness);
  RUN_TEST(test_divider_recovery_uses_nominal_ratios);
  RUN_TEST(test_two_point_fit_maps_buffers);
  RUN_TEST(test_trimmed_mean_rejects_spikes);
  RUN_TEST(test_snapshot_reset_indexes_by_metric);
  RUN_TEST(test_production_ready_requires_power_sense);
  RUN_TEST(test_snapshot_reset_sets_identity);
  RUN_TEST(test_alarm_rule_ignores_stale_sample);
  RUN_TEST(test_sgp30_crc_matches_datasheet_vector);
  RUN_TEST(test_sgp30_words_round_trip_and_reject_bad_crc);
  RUN_TEST(test_sgp30_humidity_fixed_point);
  RUN_TEST(test_ultrasonic_echo_width_to_mm);
  RUN_TEST(test_ultrasonic_range_is_2_to_400_cm);
  RUN_TEST(test_tds_datasheet_curve);
  RUN_TEST(test_tds_temperature_compensation);
  RUN_TEST(test_onewire_crc_matches_maxim_vector);
  RUN_TEST(test_ds18b20_temperature_table);
  RUN_TEST(test_ds18b20_scratchpad_checks);
  RUN_TEST(test_lamp_hysteresis_band);
  RUN_TEST(test_lamp_dwell_holds_changes);
  RUN_TEST(test_lamp_uses_pin_mv_after_lux_fit);
  RUN_TEST(test_lamp_faults_hold_then_force_off);
  RUN_TEST(test_lamp_saturated_counts_as_bright);
  RUN_TEST(test_alarm_trips_on_one_sample_and_clears_with_hysteresis);
  RUN_TEST(test_alarm_persistence_needs_sustained_valid_samples);
  RUN_TEST(test_alarm_below_rule_mirrors_above);
  RUN_TEST(test_alarm_stale_limits_per_metric);
  RUN_TEST(test_schedule_bme280_runs_four_times_per_frame);
  RUN_TEST(test_schedule_skips_missed_slots_without_burst);
  RUN_TEST(test_schedule_jitter_within_tolerance_is_not_late);
  RUN_TEST(test_schedule_defers_step_crossing_bme280_slot);
  RUN_TEST(test_safety_boot_off_until_data_and_dwell);
  RUN_TEST(test_safety_dark_turns_lamp_on_after_dwell);
  RUN_TEST(test_safety_rail_down_forces_relay_off);
  RUN_TEST(test_safety_stale_light_forces_off_without_new_sample);
  RUN_TEST(test_safety_faults_count_per_result_not_per_wake);
  RUN_TEST(test_safety_latched_interlock_beats_lamp_until_reset);
  RUN_TEST(test_safety_trip_and_clear_in_one_batch_still_deenergises);
  RUN_TEST(test_safety_disabled_rule_and_auto_switch);
  RUN_TEST(test_safety_reports_stale_rule_inputs);
  RUN_TEST(test_safety_missing_interlock_input_holds_relay_off);
  RUN_TEST(test_sticky_trip_survives_drain_during_failed_send);
  RUN_TEST(test_sticky_queued_record_is_not_duplicated);
  RUN_TEST(test_snapshot_gate_rail_marks_old_epoch_unpowered);
  RUN_TEST(test_fits_reject_non_finite_values);
  return UNITY_END();
}

// Host unit tests for the WP9 power-mode state machine (pio test -e native):
// boot paths, debounce, the quiesce -> release -> grant lease order, Wi-Fi
// acknowledgement, DEGRADED on missing acknowledgements, abort on a new loss
// during the return, and the report-only mode of the kill switch.
#include <string.h>
#include <unity.h>

#include "domain/power_mode.h"

void setUp(void) {}
void tearDown(void) {}

static const PowerModeConfig kCfg = {50, 500, 200, 2000, true};

static PowerModeFsm s_f;
static PowerModeActions s_a;
static PowerModeInputs s_in;

static void begin(bool power, const PowerModeConfig* cfg) {
  powerModeInit(&s_f, cfg);
  memset(&s_in, 0, sizeof(s_in));
  s_in.externalPower = power;
  s_in.now = 1000;
  powerModeStep(&s_f, &s_in, &s_a);
}

static void at(MonoMs now) {
  s_in.now = now;
  powerModeStep(&s_f, &s_in, &s_a);
}

static void test_boot_with_power_grants_normal_and_restores_rail_when_stable(void) {
  begin(true, &kCfg);
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_NORMAL, s_a.grant);
  TEST_ASSERT_EQUAL_UINT32(1, s_a.grantGen);
  TEST_ASSERT_FALSE(s_a.restoreRail);  // not stable yet
  at(1499);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  at(1500);
  TEST_ASSERT_TRUE(s_a.restoreRail);
  s_in.railValid = true;
  at(1600);
  TEST_ASSERT_FALSE(s_a.restoreRail);
}

static void test_boot_without_power_takes_the_low_power_path(void) {
  begin(false, &kCfg);
  TEST_ASSERT_EQUAL(POWER_WORKER_LOW, s_a.grant);
  TEST_ASSERT_TRUE(s_a.wifiStop);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_LOW, s_f.mode);
  s_in.networkOff = true;
  at(1010);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
}

// Full loss and return, every acknowledgement on time.
static void test_loss_and_return_hand_the_lease_over_in_order(void) {
  begin(true, &kCfg);
  s_in.railValid = true;
  at(2000);
  // The ISR invalidates the rail on the first edge; the level is low.
  s_in.externalPower = false;
  s_in.railValid = false;
  at(2010);
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);  // debouncing
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.quiesce);
  at(2060);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_LOW, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_NORMAL, s_a.quiesce);
  TEST_ASSERT_EQUAL_UINT32(1, s_a.quiesceGen);
  TEST_ASSERT_TRUE(s_a.wifiStop);
  at(2100);  // not released yet: no grant
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.grant);
  s_in.releasedGen = 1;
  at(2120);
  TEST_ASSERT_EQUAL(POWER_WORKER_LOW, s_a.grant);
  TEST_ASSERT_EQUAL_UINT32(2, s_a.grantGen);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_LOW, s_f.mode);  // Wi-Fi not off yet
  s_in.networkOff = true;
  at(2200);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);

  // Power back: 500 ms stable before anything moves.
  s_in.externalPower = true;
  s_in.networkOff = false;
  at(5000);
  at(5499);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
  at(5500);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_NORMAL, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_LOW, s_a.quiesce);
  TEST_ASSERT_EQUAL_UINT32(2, s_a.quiesceGen);
  s_in.releasedGen = 2;
  at(5520);
  TEST_ASSERT_TRUE(s_a.restoreRail);
  TEST_ASSERT_EQUAL(POWER_WORKER_NORMAL, s_a.grant);
  TEST_ASSERT_EQUAL_UINT32(3, s_a.grantGen);
  TEST_ASSERT_TRUE(s_a.wifiStart);
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);
}

static void test_short_glitch_does_not_change_mode_but_rail_waits_for_stability(void) {
  begin(true, &kCfg);
  s_in.railValid = true;
  at(2000);
  s_in.externalPower = false;
  s_in.railValid = false;  // the ISR saw it
  at(2010);
  s_in.externalPower = true;
  at(2030);  // 20 ms low: below the 50 ms debounce
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.quiesce);
  at(2529);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  at(2530);
  TEST_ASSERT_TRUE(s_a.restoreRail);
}

static void test_missing_release_reports_degraded_and_never_grants_early(void) {
  begin(true, &kCfg);
  s_in.externalPower = false;
  at(2000);
  at(2050);  // quiesce sent
  at(2251);  // 200 ms later, still not released
  TEST_ASSERT_EQUAL(POWER_MODE_DEGRADED, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.grant);
  at(4000);
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.grant);
  s_in.releasedGen = 1;  // late, but it came
  s_in.networkOff = true;
  at(4010);
  TEST_ASSERT_EQUAL(POWER_WORKER_LOW, s_a.grant);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
}

static void test_wifi_not_off_in_time_reports_degraded(void) {
  begin(true, &kCfg);
  s_in.externalPower = false;
  at(2000);
  at(2050);
  s_in.releasedGen = 1;
  at(2060);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_LOW, s_f.mode);
  at(4051);  // 2 s after the stop request
  TEST_ASSERT_EQUAL(POWER_MODE_DEGRADED, s_f.mode);
  s_in.networkOff = true;
  at(4100);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
}

static void test_new_loss_during_return_aborts_it(void) {
  begin(false, &kCfg);
  s_in.networkOff = true;
  at(1010);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
  s_in.externalPower = true;
  at(2000);
  at(2500);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_NORMAL, s_f.mode);
  TEST_ASSERT_EQUAL_UINT32(1, s_a.quiesceGen);
  s_in.externalPower = false;  // lost again before the release
  at(2510);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_LOW, s_f.mode);
  s_in.releasedGen = 1;
  at(2520);
  TEST_ASSERT_EQUAL(POWER_WORKER_LOW, s_a.grant);  // the low-power worker again, new generation
  TEST_ASSERT_EQUAL_UINT32(2, s_a.grantGen);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  TEST_ASSERT_FALSE(s_a.wifiStart);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
}

static void test_kill_switch_only_reports_the_level(void) {
  const PowerModeConfig off = {50, 500, 200, 2000, false};
  begin(true, &off);
  TEST_ASSERT_EQUAL(POWER_WORKER_NORMAL, s_a.grant);
  TEST_ASSERT_TRUE(s_a.restoreRail);
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);
  s_in.railValid = true;
  s_in.externalPower = false;
  at(2000);
  at(2050);
  TEST_ASSERT_EQUAL(POWER_MODE_LOW_POWER, s_f.mode);
  TEST_ASSERT_EQUAL(POWER_WORKER_NONE, s_a.quiesce);
  TEST_ASSERT_FALSE(s_a.wifiStop);
  s_in.externalPower = true;
  at(2060);
  TEST_ASSERT_EQUAL(POWER_MODE_NORMAL, s_f.mode);
  TEST_ASSERT_EQUAL_STRING("TRANSITION_TO_NORMAL", powerModeName(POWER_MODE_TRANSITION_TO_NORMAL));
}

// Audit P1: a LOW-HIGH pulse between two ticks is only visible as a new rail
// epoch. It must restart the stability timer, and the restore names the epoch.
static void test_unsampled_glitch_restarts_stability_from_the_new_epoch(void) {
  begin(true, &kCfg);
  s_in.railValid = true;
  s_in.railEpoch = 1;
  at(2000);
  s_in.railValid = false;  // the ISR saw both edges; the level is HIGH again
  s_in.railEpoch = 3;
  at(2010);
  TEST_ASSERT_FALSE(s_a.restoreRail);  // the old HIGH timer no longer counts
  at(2509);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  at(2510);
  TEST_ASSERT_TRUE(s_a.restoreRail);
  TEST_ASSERT_EQUAL_UINT32(3, s_a.restoreEpoch);
}

static void test_muted_isr_holds_the_rail_invalid(void) {
  begin(true, &kCfg);
  s_in.railValid = false;
  s_in.edgesMissed = true;  // muted after the first edge: later edges are not counted
  at(3000);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  s_in.edgesMissed = false;  // re-armed
  at(3010);
  at(3509);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  at(3510);
  TEST_ASSERT_TRUE(s_a.restoreRail);
}

static void test_glitch_during_return_defers_the_rail_restore(void) {
  begin(false, &kCfg);
  s_in.networkOff = true;
  at(1010);
  s_in.externalPower = true;
  at(2000);
  at(2500);
  TEST_ASSERT_EQUAL(POWER_MODE_TRANSITION_TO_NORMAL, s_f.mode);
  s_in.railEpoch = 2;  // an unsampled glitch while quiescing
  at(2505);
  s_in.releasedGen = 1;
  at(2510);
  TEST_ASSERT_EQUAL(POWER_WORKER_NORMAL, s_a.grant);
  TEST_ASSERT_FALSE(s_a.restoreRail);  // 5 ms of stability is not enough
  at(3004);
  TEST_ASSERT_FALSE(s_a.restoreRail);
  at(3005);
  TEST_ASSERT_TRUE(s_a.restoreRail);
  TEST_ASSERT_EQUAL_UINT32(2, s_a.restoreEpoch);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_boot_with_power_grants_normal_and_restores_rail_when_stable);
  RUN_TEST(test_boot_without_power_takes_the_low_power_path);
  RUN_TEST(test_loss_and_return_hand_the_lease_over_in_order);
  RUN_TEST(test_short_glitch_does_not_change_mode_but_rail_waits_for_stability);
  RUN_TEST(test_missing_release_reports_degraded_and_never_grants_early);
  RUN_TEST(test_wifi_not_off_in_time_reports_degraded);
  RUN_TEST(test_new_loss_during_return_aborts_it);
  RUN_TEST(test_kill_switch_only_reports_the_level);
  RUN_TEST(test_unsampled_glitch_restarts_stability_from_the_new_epoch);
  RUN_TEST(test_muted_isr_holds_the_rail_invalid);
  RUN_TEST(test_glitch_during_return_defers_the_rail_restore);
  return UNITY_END();
}

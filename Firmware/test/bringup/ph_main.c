// Bring-up entry point for ph (WP5).  pio run -e test_ph -t upload -t monitor
// Switched 5 V rail must be ON. PO -> 10k/15k -> GPIO7 (ADC1_CH6).
// Replaces src/main.c in this env only; links the production driver.
//
// Procedure (README "pH-4502C calibration"). Every reading is followed by a
// "module NNNN mV" line: the module PO voltage the buffer fit is made from,
// whatever the pH line shows (nominal fit, buffer fit or none).
//  1. Offset: probe off, BNC shorted. Turn the pot nearest the BNC until the
//     module line reads ~2500 mV - that is the module PO voltage (pH 7).
//     A factory-set module starts near 0 V and prints "ph=UNPOWERED": check
//     the 5 V rail, then keep turning the pot until mV values appear.
//  2. Probe in, rinse, pH 6.86 buffer, wait >= 2 min, note the module mV.
//     Rinse, pH 4.01 buffer, wait >= 2 min, note the module mV.
//  3. Put both mV values below and re-flash: readings switch to pH / OK.
// Without buffers: pH comes from the nominal fit below and prints "(uncal)".
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/adc_channel.h"
#include "domain/calibration.h"
#include "drivers/ph.h"

// Module mV measured in each buffer (step 2). 0 = not measured yet: uncalibrated.
#define PH_BUFFER_A_PH 6.86f
#define PH_BUFFER_A_MV 0.0f
#define PH_BUFFER_B_PH 4.01f
#define PH_BUFFER_B_MV 0.0f
#define PH_CALIBRATION_REVISION 1

// Nominal fit, used until both buffer values are set. Offset: module mV with the
// BNC shorted (pH 7), measured 17 Oct 2026. Re-measure if the offset pot moves.
// Slope: tutorial sketch, pH = -5.70 * V. Accuracy roughly +/-0.5-1 pH.
#define PH_NOMINAL_MV_AT_PH7 2758.0f
#define PH_NOMINAL_PH_PER_MV (-0.0057f)

static AdcChannel s_adc = ADC_CHANNEL_INIT;
static PhConfig s_config = {&s_adc, BOARD_PH_DIVIDER_TOP_OHM, BOARD_PH_DIVIDER_BOTTOM_OHM,
                            {1.0f, 0.0f, 0, false}, false};  // set in setup()
static PhDriver s_driver;
// The module mV for the procedure, from the pin mV the driver publishes as raw.
static void printModuleMv(SensorDriver* driver) {
  Reading r;
  if (sensorDriverReadResult(driver, &r, 1) != 1 || !r.hasRaw) return;
  printf("    module %.0f mV\n",
         (double)dividerRecoverMv((float)r.raw, BOARD_PH_DIVIDER_TOP_OHM, BOARD_PH_DIVIDER_BOTTOM_OHM));
}

static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, printModuleMv};

void setup(void) {
  if (PH_BUFFER_A_MV > 0.0f && PH_BUFFER_B_MV > 0.0f) {
    linearFromTwoPoints(PH_BUFFER_A_MV, PH_BUFFER_A_PH, PH_BUFFER_B_MV, PH_BUFFER_B_PH,
                        PH_CALIBRATION_REVISION, &s_config.calibration);
  } else {
    s_config.calibration.slope = PH_NOMINAL_PH_PER_MV;
    s_config.calibration.offset = 7.0f - PH_NOMINAL_PH_PER_MV * PH_NOMINAL_MV_AT_PH7;
    s_config.calibration.revision = 0;
    s_config.calibration.valid = true;
    s_config.calibrationProvisional = true;
  }
  phDriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: %s ===\n", sensorDriverName(&s_driver.base));
  if (!adcChannelBegin(&s_adc, BOARD_PIN_PH_ADC)) {
    printf("GPIO%d is not an ADC1 pin\n", (int)BOARD_PIN_PH_ADC);
  }
  if (s_config.calibrationProvisional) {
    printf("nominal pH (not buffer-calibrated): pH 7 at %.0f mV, %.4f pH/mV\n",
           (double)PH_NOMINAL_MV_AT_PH7, (double)PH_NOMINAL_PH_PER_MV);
  } else if (s_config.calibration.valid) {
    // Tutorial nominal is about -0.0057 pH/mV; far from it means a bad buffer reading.
    printf("calibration rev %u: pH = %.6f * mV + %.3f\n",
           (unsigned)s_config.calibration.revision, (double)s_config.calibration.slope,
           (double)s_config.calibration.offset);
  } else {
    printf("uncalibrated: printing module PO voltage in mV\n");
  }
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

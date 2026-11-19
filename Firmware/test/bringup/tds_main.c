// Bring-up entry point for tds (WP5).  pio run -e test_tds -t upload -t monitor
// Switched 5 V rail must be ON. A -> GPIO6 (ADC1_CH5), 100 nF to GND.
// Replaces src/main.c in this env only; links the production driver.
//
// Procedure (README "SEN0244 TDS bring-up"):
//  1. Probe in the liquid, away from the container wall (datasheet), stir
//     gently and wait for the reading to settle. Water below 55 degC only.
//  2. Without a water temperature every line is "tds=DEPENDENCY_INVALID
//     (raw <pin mV>)": this env runs the TDS driver alone, without the DS18B20.
//  3. Measure the water with a thermometer, put it in TDS_MANUAL_WATER_TEMP_C
//     and re-flash: lines switch to "<ppm> ppm (uncal)" (nominal curve).
// "tds=FAULT (raw ...)" means above 1000 ppm (raw >~ 2160 mV at 25 degC), not
// a wiring fault: an ADC error shows as "poll -> FAULT" instead.
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/adc_channel.h"
#include "hal/clock.h"
#include "drivers/tds.h"

// Water temperature from a thermometer, degC. 0 = off (no compensation).
// BRING-UP ONLY: stands in for the DS18B20 and is re-stamped as fresh every
// frame. Production feeds the real DS18B20 reading.
#define TDS_MANUAL_WATER_TEMP_C 0.0f

static AdcChannel s_adc = ADC_CHANNEL_INIT;
static TdsConfig s_config = {&s_adc, {1.0f, 0.0f, 0, false}};  // nominal curve, no K-factor
static TdsDriver s_driver;

// Feed the manual temperature as a fresh OK DS18B20 reading.
static void feedManualTemperature(SensorDriver* driver) {
  (void)driver;
  Reading t;
  readingReset(&t, SENSOR_DS18B20, METRIC_WATER_TEMPERATURE, UNIT_DEG_C);
  const MonoMs now = monoNowMs();
  t.health = HEALTH_OK;
  t.value = TDS_MANUAL_WATER_TEMP_C;
  t.intervalStartMs = now;
  t.intervalEndMs = now;
  t.lastGoodMs = now;
  tdsDriverSetWaterTemperature(&s_driver, &t);
}

static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, NULL};

void setup(void) {
  tdsDriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: %s ===\n", sensorDriverName(&s_driver.base));
  if (!adcChannelBegin(&s_adc, BOARD_PIN_TDS_ADC)) {
    printf("GPIO%d is not an ADC1 pin\n", (int)BOARD_PIN_TDS_ADC);
  }
  if (TDS_MANUAL_WATER_TEMP_C > 0.0f) {
    printf("manual water temperature %.1f degC (bring-up only, not a DS18B20)\n",
           (double)TDS_MANUAL_WATER_TEMP_C);
    feedManualTemperature(&s_driver.base);
    s_ctx.afterResult = feedManualTemperature;
  } else {
    printf("no water temperature: readings are DEPENDENCY_INVALID, raw = pin mV\n");
  }
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

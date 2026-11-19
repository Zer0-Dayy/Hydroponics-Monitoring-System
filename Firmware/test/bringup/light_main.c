// Bring-up entry point for light (WP3).  pio run -e test_light -t upload -t monitor
// Retained 3.3 V rail. S -> 10 kOhm -> GPIO1 (ADC1_CH0), 1 uF to GND.
// Replaces src/main.c in this env only; links the production driver.
//
// Every line prints module mV with "(uncal)": there is no lux mapping (D13).
// Procedure (README "DFR0026 light bring-up"), note the mV for each step:
//  1. Dark: cover the sensor completely.
//  2. Room light, sensor uncovered.
//  3. Phone torch close to the lens: find where it clips (~3050 mV at the pin;
//     the reading carries kQualitySaturated).
//  4. Under LED or fluorescent light: note the spread between successive lines
//     (flicker rejection of the 11 ms tap spacing).
//  5. Unplug S: watch how the reading decays (the 1 uF holds the last level).
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/adc_channel.h"
#include "drivers/light.h"

static AdcChannel s_adc = ADC_CHANNEL_INIT;
static LightConfig s_config = {&s_adc};
static LightDriver s_driver;
static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, NULL};

void setup(void) {
  lightDriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: %s ===\n", sensorDriverName(&s_driver.base));
  if (!adcChannelBegin(&s_adc, BOARD_PIN_LIGHT_ADC)) {
    printf("GPIO%d is not an ADC1 pin\n", (int)BOARD_PIN_LIGHT_ADC);
  }
  printf("%d taps, %d ms apart, trimmed mean; printing module mV (uncal)\n",
         LIGHT_SAMPLES, LIGHT_SAMPLE_SPACING_MS);
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

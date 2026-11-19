// Bring-up: DFR0026 light level -> relay module -> grow lamp.
//   pio run -e test_lamp -t upload -t monitor
// Light as in test_light (S -> 10 kOhm -> GPIO1, 1 uF to GND). Relay: a
// TONGLING JQC-3FF-S-Z 5 V "Low Level Trigger" module, VCC 5 V, GND common.
// IN is driven through an N-MOSFET or NPN like the PCB stage (GPIO15 -> 220 Ohm
// gate / 1 kOhm base, 10 kOhm pull-down, IN on drain/collector), so GPIO15
// HIGH = relay ON: active-high, as BOARD_RELAY1_ACTIVE_LOW false. Direct drive
// (IN on GPIO15) failed on hardware 24 Oct 2026: 3.3 V cannot release the
// module's 5 V-referenced input, the relay stayed on with GPIO15 HIGH, and an
// unpowered module back-fed ~2.5 V onto VCC through IN.
// Lamp live on COM -> NO, NC empty: relay off = lamp off.
//
// Not the production path (WP7: Safety owns the relay). The decision is
// domain/lamp_control; this file only feeds it readings and drives the pin.
// One line per reading, e.g.
//   [lamp] 32 mV (uncal) -> ON  held 41 s
//   [lamp] 150 mV (uncal) -> ON  held 12 s, OFF waiting (dwell 30 s)
// LAMP_* thresholds are bench placeholders; set them from the lamp-off /
// lamp-on readings at the final sensor position (README "Lamp relay bring-up").
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "domain/lamp_control.h"
#include "drivers/light.h"
#include "hal/adc_channel.h"
#include "hal/clock.h"

#define LAMP_RELAY_ACTIVE_LOW 0  // transistor stage; 1 only for a module that releases at 3.3 V
#define LAMP_ON_BELOW_MV 40.0f
#define LAMP_OFF_ABOVE_MV 120.0f
#define LAMP_MIN_ON_MS ((uint32_t)30000)   // bench; minutes in real use
#define LAMP_MIN_OFF_MS ((uint32_t)30000)
#define LAMP_FAULTS_TO_OFF 5

static AdcChannel s_adc = ADC_CHANNEL_INIT;
static LightConfig s_config = {&s_adc};
static LightDriver s_driver;
static LampControl s_lamp;

static void relayWrite(bool on) {
  const bool high = LAMP_RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(BOARD_PIN_RELAY1, high ? HIGH : LOW);
}

static void afterLight(SensorDriver* driver) {
  Reading r;
  sensorDriverReadResult(driver, &r, 1);
  const MonoMs now = monoNowMs();
  const bool wasOn = s_lamp.on;
  const bool on = lampControlUpdate(&s_lamp, &r, now);
  relayWrite(on);

  printf("[lamp] ");
  if (r.health == HEALTH_OK || r.health == HEALTH_UNCALIBRATED) {
    printf("%.0f mV%s", (double)r.value, (r.qualityFlags & kQualitySaturated) ? " SAT" : "");
  } else {
    printf("light=%s (%u in a row)", healthName(r.health), (unsigned)s_lamp.faults);
  }
  printf(" -> %s  held %lu s", on ? "ON " : "OFF",
         (unsigned long)(lampControlHeldMs(&s_lamp, now) / 1000));
  if (s_lamp.waiting) {
    printf(", %s waiting (dwell %lu s)", on ? "OFF" : "ON",
           (unsigned long)((on ? LAMP_MIN_ON_MS : LAMP_MIN_OFF_MS) / 1000));
  }
  if (on != wasOn) printf("   <-- relay %s", on ? "ON" : "OFF");
  printf("\n");
}

static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, afterLight};

void setup(void) {
  // Relay off before the pin becomes an output, so the lamp never blinks.
  relayWrite(false);
  pinMode(BOARD_PIN_RELAY1, OUTPUT);
  relayWrite(false);

  static const LampControlConfig lampConfig = {LAMP_ON_BELOW_MV, LAMP_OFF_ABOVE_MV,
                                               LAMP_MIN_ON_MS, LAMP_MIN_OFF_MS,
                                               LAMP_FAULTS_TO_OFF};
  lampControlInit(&s_lamp, &lampConfig, monoNowMs());
  lightDriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: lamp (light GPIO%d -> relay GPIO%d, active-%s) ===\n",
         (int)BOARD_PIN_LIGHT_ADC, (int)BOARD_PIN_RELAY1, LAMP_RELAY_ACTIVE_LOW ? "low" : "high");
  printf("ON below %.0f mV, OFF above %.0f mV, min on/off %lu/%lu s, OFF after %d bad readings\n",
         (double)LAMP_ON_BELOW_MV, (double)LAMP_OFF_ABOVE_MV,
         (unsigned long)(LAMP_MIN_ON_MS / 1000), (unsigned long)(LAMP_MIN_OFF_MS / 1000),
         LAMP_FAULTS_TO_OFF);
  if (!adcChannelBegin(&s_adc, BOARD_PIN_LIGHT_ADC)) {
    printf("GPIO%d is not an ADC1 pin\n", (int)BOARD_PIN_LIGHT_ADC);
  }
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

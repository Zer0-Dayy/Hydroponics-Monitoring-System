// Bring-up entry point for hcsr04 (WP4).  pio run -e test_hcsr04 -t upload -t monitor
// Switched 5 V rail must be ON. Verify the 1k/2k ECHO divider first.
// Replaces src/main.c in this env only; links the production driver.
//
// Procedure (README "HC-SR04 bring-up"), note the line printed for each step:
//  0. Divider gate, before connecting GPIO18: with the module's ECHO wire
//     disconnected, put 5 V on the module side of the 1 kOhm and measure ~3.3 V
//     at the divider junction (or scope the junction on a running module).
//     Never force the module's ECHO output to 5 V: it is a driven output.
//  1. Flat target at 10, 50 and 100 cm: "[hcsr04] 1000.xx mm"; compare with a tape.
//  2. Point at open space (> 4 m): "water_distance=TIMEOUT (raw <us>)".
//  3. Unplug ECHO at the MODULE side (the 2 kOhm keeps GPIO18 low):
//     "water_distance=TIMEOUT", no raw. Not at the GPIO18 side: the pin has no
//     pull on purpose and would float. Do not test with the 5 V rail off while
//     this harness runs - it keeps driving TRIG high into the unpowered module.
//  4. Hold ECHO high (to 3.3 V on the GPIO side): "init -> FAULT".
//  5. Target closer than 2 cm: FAULT (raw = width) or an unstable value.
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/pulse_capture.h"
#include "drivers/hcsr04.h"

static PulseCapture s_capture = PULSE_CAPTURE_INIT;
static Hcsr04Config s_config = {&s_capture, CFG_HCSR04_TIMEOUT_MS};
static Hcsr04Driver s_driver;
static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, NULL};

void setup(void) {
  hcsr04DriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: %s ===\n", sensorDriverName(&s_driver.base));
  if (!pulseCaptureBegin(&s_capture, BOARD_PIN_HCSR04_TRIG, BOARD_PIN_HCSR04_ECHO)) {
    printf("pulse capture: invalid pins TRIG=%d ECHO=%d\n", (int)BOARD_PIN_HCSR04_TRIG,
           (int)BOARD_PIN_HCSR04_ECHO);
  }
  printf("TRIG GPIO%d, ECHO GPIO%d, timeout %lu ms, nominal 343 m/s\n",
         (int)BOARD_PIN_HCSR04_TRIG, (int)BOARD_PIN_HCSR04_ECHO,
         (unsigned long)CFG_HCSR04_TIMEOUT_MS);
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

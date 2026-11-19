// Bring-up entry point for ds18b20 (WP2).  pio run -e test_ds18b20 -t upload -t monitor
// Retained 3.3 V supply, DQ on GPIO14 with a 4.7 kOhm pull-up to 3.3 V.
// 12-bit conversion (750 ms max) runs inside the 1 s period.
// Replaces src/main.c in this env only; links the production driver.
//
// Prints the selected ROM code once, then one temperature per second.
// "init -> NOT_PRESENT": no presence pulse (DQ open, no supply, missing
// pull-up) or the first device is not a DS18B20 (family 28h).
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/onewire_bus.h"
#include "drivers/ds18b20.h"

static OneWireBus s_bus = ONE_WIRE_BUS_INIT;
static Ds18b20Config s_config = {&s_bus};
static Ds18b20Driver s_driver;

// Print the ROM code whenever init() selected a different one.
static void printRomIfNew(SensorDriver* driver) {
  (void)driver;
  static uint8_t last[8];
  static bool printed = false;
  uint8_t rom[8];
  if (!ds18b20DriverGetRom(&s_driver, rom)) return;
  if (printed && memcmp(rom, last, sizeof(rom)) == 0) return;
  memcpy(last, rom, sizeof(rom));
  printed = true;
  printf("# ds18b20 ROM %02X-%02X%02X%02X%02X%02X%02X crc %02X\n", rom[0], rom[6], rom[5],
         rom[4], rom[3], rom[2], rom[1], rom[7]);
}

static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, printRomIfNew};

void setup(void) {
  ds18b20DriverConstruct(&s_driver, &s_config);
  delay(200);
  printf("\n=== bring-up: %s ===\n", sensorDriverName(&s_driver.base));
  if (!oneWireBusBegin(&s_bus, BOARD_PIN_ONE_WIRE)) {
    printf("GPIO%d cannot be used for 1-Wire\n", (int)BOARD_PIN_ONE_WIRE);
  }
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

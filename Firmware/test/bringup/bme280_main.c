// Bring-up entry point for bme280 (WP1).  pio run -e test_bme280 -t upload -t monitor
// 3.3 V rail. Expect 3 readings every 250 ms, took <= 12 ms.
// Replaces src/main.c in this env only; links the production driver.
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/i2c_bus.h"
#include "drivers/bme280.h"

static I2cBus s_i2c = I2C_BUS_INIT;
static Bme280Config s_config;  // address chosen at runtime in setup()
static Bme280Driver s_driver;
static BringupContext s_ctx = {&s_driver.base, CFG_BME280_PERIOD_MS, NULL};

static const char* chipName(uint8_t id) {
  if (id == 0x60) return "BME280";
  if (id == 0x56 || id == 0x57 || id == 0x58) return "BMP280 (no humidity!)";
  return "unknown";
}

void setup(void) {
  delay(200);
  printf("\n=== bring-up: bme280 (SDA %d, SCL %d, %lu Hz) ===\n", BOARD_PIN_I2C_SDA,
         BOARD_PIN_I2C_SCL, (unsigned long)BOARD_I2C_CLOCK_HZ);

  const bool busOk = i2cBusBegin(&s_i2c, BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL,
                                 BOARD_I2C_CLOCK_HZ);
  printf("i2c begin -> %s\n", busOk ? "OK" : "FAILED");

  // Pick whichever address ACKs (SDO strap); default if neither does.
  uint8_t address = BOARD_BME280_I2C_ADDRESS;
  const bool ack76 = i2cBusProbe(&s_i2c, BOARD_BME280_I2C_ADDRESS);
  const bool ack77 = i2cBusProbe(&s_i2c, BOARD_BME280_I2C_ADDRESS_ALT);
  printf("probe 0x%02X -> %s, 0x%02X -> %s\n", BOARD_BME280_I2C_ADDRESS,
         ack76 ? "ACK" : "-", BOARD_BME280_I2C_ADDRESS_ALT, ack77 ? "ACK" : "-");
  if (!ack76 && ack77) address = BOARD_BME280_I2C_ADDRESS_ALT;

  uint8_t id = 0;
  if (i2cBusReadRegs(&s_i2c, address, 0xD0, &id, 1)) {
    printf("chip id @0x%02X = 0x%02X (%s)\n", address, id, chipName(id));
  } else {
    printf("chip id @0x%02X -> read failed (check wiring/pull-ups)\n", address);
  }

  s_config.bus = &s_i2c;
  s_config.address = address;
  bme280DriverConstruct(&s_driver, &s_config);
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

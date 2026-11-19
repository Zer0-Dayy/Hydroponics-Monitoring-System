// Bring-up entry point for sgp30 (WP6).  pio run -e test_sgp30 -t upload -t monitor
// Retained 3.3 V supply, shared I2C bus. Expect 2 readings every 1 s, ~13 ms
// each. The first 15 s after init are WARMING (fixed 400 ppm / 0 ppb), never
// published. Without a restored baseline the values then print "(uncal)"
// (baseline LEARNING, D13) for 12 h; after that the baseline is VALID, the
// values turn OK and the baseline words are printed once an hour.
// Humidity compensation is not used here (one driver per harness), so OK
// values are uncompensated.
// Replaces src/main.c in this env only; links the production driver.
#include <Arduino.h>
#include <stdio.h>

#include "app_config.h"
#include "board_profile.h"
#include "bringup_harness.h"
#include "hal/i2c_bus.h"
#include "drivers/sgp30.h"

// Baseline to restore at init, as printed by an earlier run ("baseline co2eq
// 0x.... tvoc 0x...."). Both 0 = none: the sensor learns from scratch.
// Sensirion: a stored baseline is valid for up to 7 days with the sensor off.
#define SGP30_RESTORE_CO2EQ 0x0000
#define SGP30_RESTORE_TVOC 0x0000

static I2cBus s_i2c = I2C_BUS_INIT;
static Sgp30Config s_config = {&s_i2c, BOARD_SGP30_I2C_ADDRESS};
static Sgp30Driver s_driver;

static Sgp30BaselineStatus s_lastStatus = SGP30_BASELINE_NONE;
static uint16_t s_lastCo2eq = 0;
static uint16_t s_lastTvoc = 0;

static void printBaseline(SensorDriver* d) {
  (void)d;
  const Sgp30BaselineStatus st = sgp30DriverBaselineStatus(&s_driver);
  if (st != s_lastStatus) {
    printf("[sgp30] baseline %s (feature set 0x%04X)\n", sgp30BaselineStatusName(st),
           (unsigned)sgp30DriverFeatureSet(&s_driver));
    s_lastStatus = st;
  }
  uint16_t co2eq = 0;
  uint16_t tvoc = 0;
  if (sgp30DriverGetBaseline(&s_driver, &co2eq, &tvoc) &&
      (co2eq != s_lastCo2eq || tvoc != s_lastTvoc)) {
    printf("[sgp30] baseline co2eq 0x%04X tvoc 0x%04X\n", (unsigned)co2eq, (unsigned)tvoc);
    s_lastCo2eq = co2eq;
    s_lastTvoc = tvoc;
  }
}

static BringupContext s_ctx = {&s_driver.base, CFG_FRAME_PERIOD_MS, printBaseline};

void setup(void) {
  sgp30DriverConstruct(&s_driver, &s_config);
  if (SGP30_RESTORE_CO2EQ != 0 || SGP30_RESTORE_TVOC != 0) {
    sgp30DriverSetBaseline(&s_driver, SGP30_RESTORE_CO2EQ, SGP30_RESTORE_TVOC);
  }
  delay(200);
  printf("\n=== bring-up: %s (SDA %d, SCL %d) ===\n", sensorDriverName(&s_driver.base),
         BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
  i2cBusBegin(&s_i2c, BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_I2C_CLOCK_HZ);
  printf("probe 0x%02X -> %s\n", BOARD_SGP30_I2C_ADDRESS,
         i2cBusProbe(&s_i2c, BOARD_SGP30_I2C_ADDRESS) ? "ACK" : "-");
  if (SGP30_RESTORE_CO2EQ != 0 || SGP30_RESTORE_TVOC != 0) {
    printf("restoring baseline co2eq 0x%04X tvoc 0x%04X\n", (unsigned)SGP30_RESTORE_CO2EQ,
           (unsigned)SGP30_RESTORE_TVOC);
  }
  bringupStart(&s_ctx);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }

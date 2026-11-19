// bme280 driver. Work package WP1 (Stage 2). Architecture §4, §7, §14.
// Datasheet: Bosch BST-BME280-DS001 rev 1.24.
//
// Verify chip ID 0x60 (rejects BMP280); read compensation coefficients once
// in init(). Forced mode, x1 oversampling T/P/H, IIR off; Bosch integer
// compensation (§4.2.3). Produces AirTemperature, AirHumidity, AirPressure.
//
// Worst-case blocking (I2C_BUS_TIMEOUT_MS = T per transaction):
//   init()    ~13 ms + 16 T  (soft reset + bounded NVM-copy wait)
//   start()   1 T            (one register write)
//   poll()    2 T            (ctrl_hum/status + one 8-byte burst); Pending
//                            until start+10 ms (t_max 9.3 ms), Timeout
//                            after CFG_BME280_BUDGET_MS
//   others    no bus access
// Any bus fault, timeout or invalid sample clears the initialised state, so
// the next start() returns Fault and the caller re-runs init(). poll() also
// detects a module power-cycle (ctrl_hum back to reset 0): it fails the
// sample and clears the initialised state; the caller then re-runs init().
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/i2c_bus.h"

typedef struct Bme280Config {
  I2cBus* bus;
  uint8_t address;  // BOARD_BME280_I2C_ADDRESS
} Bme280Config;

#define BME280_METRICS 3  // results_ index: 0 T, 1 H, 2 P

typedef struct Bme280Calib {
  uint16_t t1;
  int16_t t2, t3;
  uint16_t p1;
  int16_t p2, p3, p4, p5, p6, p7, p8, p9;
  uint8_t h1, h3;
  int16_t h2, h4, h5;
  int8_t h6;
} Bme280Calib;

// Members after `base` are private to bme280.c.
typedef struct Bme280Driver {
  SensorDriver base;  // must stay first
  Bme280Config config_;
  Bme280Calib calib_;
  int32_t tFine_;
  MonoMs startMs_;
  bool initialised_;
  bool busy_;
  Reading results_[BME280_METRICS];
} Bme280Driver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void bme280DriverConstruct(Bme280Driver* self, const Bme280Config* config);

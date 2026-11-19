// sgp30 driver. Work package WP6 (Stage 2). Architecture §9, §10, §16 (D13, D18).
// Datasheet: Sensirion SGP30 v1.0, May 2020 (E13; local copy in
// Electrical System Study/Datasheets/Sensirion_Gas_Sensors_Datasheet_SGP30.pdf).
//
// Retained 3.3 V supply, shared I2C bus with the BME280 (address 0x58). The
// Adafruit breakout regulates the chip's 1.8 V and shifts the I2C levels.
// Produces Tvoc (ppb) and Co2eq (ppm, an estimate - never a CO2 measurement).
//
// init() checks the feature set (product type 0), sends iaq_init and restores
// a baseline if one is known. Then one measure_iaq per second from the
// absolute frame: start() sends the command, poll() reads 6 bytes once the
// 12 ms conversion is over and checks the CRC-8 of each word. The sensor NACKs
// while it converts, so it is read once, never polled.
//
// Health, highest priority first:
//   FAULT              CRC or bus error on this measurement (also while warming)
//   WARMING            first 15 s after iaq_init (fixed 400 ppm / 0 ppb)
//   DEPENDENCY_INVALID humidity compensation enabled but its input is stale
//   UNCALIBRATED       baseline still LEARNING (no restore, < 12 h) - D13
//   OK                 otherwise; kQualityUncompensated if no humidity input
//
// Unlike the BME280, a single fault does NOT force a re-init: re-init means a
// new iaq_init and 15 s of WARMING. CRC/bus errors and missed seconds (start()
// later than 1.8 s after the previous one) count as gaps; after 3 consecutive
// gaps start() returns Fault and the caller re-runs init(), which re-applies
// the last known baseline.
//
// Baseline persistence belongs to Configuration (WP8, NVS). This driver only
// keeps a RAM copy: it is fetched hourly once VALID or RESTORED, read with
// sgp30DriverGetBaseline() and restored at init from sgp30DriverSetBaseline().
//
// Worst-case blocking (I2C_BUS_TIMEOUT_MS = T per transaction):
//   init()    ~30 ms + 4 T  (feature set, iaq_init, set_iaq_baseline, each
//                            followed by its 10 ms maximum duration)
//   start()   1 T           (one command write)
//   poll()    2 T           (6-byte read + humidity write or baseline command);
//                            Pending until start+13 ms; once an hour Pending
//                            for 11 ms more to read the baseline (1 T)
//   others    no bus access
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/i2c_bus.h"

typedef struct Sgp30Config {
  I2cBus* bus;
  uint8_t address;  // BOARD_SGP30_I2C_ADDRESS
} Sgp30Config;

#define SGP30_METRICS 2  // results_ index: 0 TVOC, 1 CO2eq

// Baseline compensation state, reported separately from the readings (§10).
typedef uint8_t Sgp30BaselineStatus;
enum {
  SGP30_BASELINE_NONE = 0,  // not initialised yet
  SGP30_BASELINE_LEARNING,  // no baseline restored; learning for 12 h
  SGP30_BASELINE_RESTORED,  // stored baseline applied after iaq_init
  SGP30_BASELINE_VALID      // learned for 12 h without a restart
};

// Members after `base` are private to sgp30.c.
typedef struct Sgp30Driver {
  SensorDriver base;  // must stay first
  Sgp30Config config_;
  bool initialised_;
  uint8_t step_;             // measurement step (sgp30.c)
  uint8_t consecutiveGaps_;
  uint16_t featureSet_;
  MonoMs startMs_;
  MonoMs initMs_;
  MonoMs lastStartMs_;
  MonoMs baselineReadyMs_;   // when the baseline answer can be read
  MonoMs baselineFetchDueMs_;
  Sgp30BaselineStatus baselineStatus_;
  MonoMs learningStartMs_;
  bool hasBaseline_;         // baselineCo2eq_/baselineTvoc_ hold a usable baseline
  uint16_t baselineCo2eq_;
  uint16_t baselineTvoc_;
  bool hasPendingBaseline_;  // set by sgp30DriverSetBaseline(), applied at init
  uint16_t pendingCo2eq_;
  uint16_t pendingTvoc_;
  bool humidityEnabled_;     // sgp30DriverSetHumidity() has been called
  bool humidityValid_;       // its last input was fresh
  bool humidityDirty_;       // humidity8p8_ not yet written to the chip
  uint16_t humidity8p8_;     // g/m3 in 8.8 fixed point, 0 = compensation off
  MonoMs humidityInputMs_;   // intervalEndMs of the older of the two inputs
  Reading results_[SGP30_METRICS];
} Sgp30Driver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void sgp30DriverConstruct(Sgp30Driver* self, const Sgp30Config* config);

// Humidity compensation input from the latest BME280 temperature and humidity
// readings (copied, not shared). Both must be OK and fresh at `now`
// (CFG_STALE_BME280_MS); otherwise compensation is switched off on the chip
// (written as 0) and the readings report DependencyInvalid. The same happens
// if the caller stops calling this and the last input ages past that limit.
// Written after the next measurement (changes under ~0.03 g/m3 are ignored);
// no bus access here.
void sgp30DriverSetHumidity(Sgp30Driver* self, const Reading* airTemperature,
                            const Reading* airHumidity, MonoMs now);

// Stored baseline (from NVS) to restore at the next init(). Values in the
// order get_iaq_baseline returns them.
void sgp30DriverSetBaseline(Sgp30Driver* self, uint16_t co2eq, uint16_t tvoc);

// Latest baseline worth storing. false while LEARNING (Sensirion: do not
// store a baseline before 12 h unless one was restored).
bool sgp30DriverGetBaseline(const Sgp30Driver* self, uint16_t* co2eq, uint16_t* tvoc);

Sgp30BaselineStatus sgp30DriverBaselineStatus(const Sgp30Driver* self);
const char* sgp30BaselineStatusName(Sgp30BaselineStatus s);

// Feature set word read in init() (0 until then), e.g. 0x0022.
uint16_t sgp30DriverFeatureSet(const Sgp30Driver* self);

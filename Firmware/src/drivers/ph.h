// ph4502c driver. Work package WP5 (Stage 3). Architecture §3, §7, §14.
// Source: PH-Sensor-4502C.pdf (a tutorial, not a manufacturer specification).
//
// Switched 5 V rail. PO -> 10k/15k divider -> ADC1 pin (GPIO7). DO/TO unused.
// Pin mV -> divider recovery -> module mV -> two-point buffer fit -> pH.
// Module output is 0-5 V, so at most 3 V at the pin (inside the S3's ~3.1 V
// range at 11 dB). With the offset pot set so a shorted BNC reads 2.5 V at PO
// (pH 7), the tutorial's nominal slope is about -5.7 pH/V.
//
// Filter (tutorial sketch): PH_SAMPLES conversions spaced PH_SAMPLE_SPACING_MS
// apart, sorted, mean of the middle PH_SAMPLES - 2*PH_TRIM. The window is spread
// over successive poll() calls, never taken as a burst (§9): each tap is due
// PH_SAMPLE_SPACING_MS after the previous actual sample, so a late poll delays
// the rest of the window instead of compressing it.
// No temperature compensation: the tutorial reports < 0.01 pH below 30 degC.
//
// Results:
//   calibration.valid  -> value in pH, HEALTH_OK (FAULT outside 0-14)
//     + calibrationProvisional -> value in pH, HEALTH_UNCALIBRATED (a nominal
//                         fit, e.g. shorted-BNC offset + tutorial slope)
//                         (also outside 0-14: the nominal fit may not match the
//                         probe yet; capturable for calibration, never actionable)
//   otherwise          -> value = module mV, UNIT_MILLIVOLT, HEALTH_UNCALIBRATED
//   window mean < PH_NO_SIGNAL_PIN_MV -> HEALTH_UNPOWERED (a 0 V output is a
//                         dead rail or open lead, never "very acidic", §10).
//                         Also seen before the offset pot is set: factory
//                         offset puts pH 7 near 0 V.
//   any sample clipped -> kQualitySaturated + HEALTH_FAULT
// poll() returns READY for all of these; the health carries the verdict.
// raw = filtered pin mV in every case.
//
// Worst-case blocking (one ADC conversion C, well under 1 ms):
//   init()    1 C   (throwaway conversion, checks the channel)
//   start()   1 C   (first sample of the window)
//   poll()    1 C   Pending until the window is full (>= 270 ms)
//   others    none
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/adc_channel.h"
#include "domain/calibration.h"

#define PH_SAMPLES 10
#define PH_TRIM 2
#define PH_SAMPLE_SPACING_MS 30
// Provisional: below this at the pin the module is treated as unpowered.
// pH 14 is ~1.3 V at PO (~0.8 V at the pin), so 100 mV is far from real data.
#define PH_NO_SIGNAL_PIN_MV 100

typedef struct PhConfig {
  AdcChannel* adc;             // already begun on BOARD_PIN_PH_ADC
  uint32_t dividerTopOhm;      // BOARD_PH_DIVIDER_TOP_OHM
  uint32_t dividerBottomOhm;   // BOARD_PH_DIVIDER_BOTTOM_OHM
  LinearCalibration calibration;  // x = module mV, y = pH
  bool calibrationProvisional;    // true: fit not checked against buffers
} PhConfig;

// Members after `base` are private to ph.c.
typedef struct PhDriver {
  SensorDriver base;  // must stay first
  PhConfig config_;
  bool initialised_;
  bool busy_;
  bool saturated_;
  uint8_t count_;
  uint32_t samples_[PH_SAMPLES];  // pin mV
  MonoMs startMs_;
  MonoMs lastSampleMs_;  // next tap is due PH_SAMPLE_SPACING_MS after this
  Reading result_;
} PhDriver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void phDriverConstruct(PhDriver* self, const PhConfig* config);

// New fit from the applied configuration (on-device calibration, WP8).
// Called by the acquisition owner between windows; applies from the next one.
void phDriverSetCalibration(PhDriver* self, const LinearCalibration* calibration, bool provisional);

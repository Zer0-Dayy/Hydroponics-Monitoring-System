// sen0244_tds driver. Work package WP5 (Stage 3). Architecture §8, §14.
// Source: DFRobot Gravity Analog TDS Sensor SEN0244 datasheet (wiki + sample).
//
// Switched 5 V rail. A -> GPIO6 (ADC1_CH5), 100 nF to GND, no divider: pin mV
// = module mV. Output 0-2.3 V nominal, inside the S3's ~3.1 V range at 11 dB.
// Pin mV -> datasheet cubic -> temperature compensation of the ppm (EC, as
// DFRobot GravityTDS; domain/tds_codec.h)
// -> optional K-factor (calibration: x = nominal ppm, y = ppm) -> ppm.
//
// Filter: TDS_SAMPLES conversions spaced TDS_SAMPLE_SPACING_MS apart, sorted,
// mean of the middle TDS_SAMPLES - 2*TDS_TRIM (stands in for the sample's
// median). The sample takes 30 taps 40 ms apart (~1.2 s), which overruns the
// 1 s frame, so the window is shortened to ~560 ms. Spread over successive
// poll() calls, never a burst (§9), spacing measured from the previous tap.
//
// Temperature compensation needs a fresh DS18B20 reading via
// tdsDriverSetWaterTemperature(): OK and not older than CFG_STALE_ORDINARY_MS
// at the end of the window, within 0-55 degC (probe limit). Otherwise the
// value is computed uncompensated - never silently assumed to be 25 degC.
//
// Results (unit ppm, raw = filtered pin mV in every case), highest first:
//   any tap clipped or ppm > 1000 -> kQualitySaturated + HEALTH_FAULT (value
//                         kept; above the rated range, never actionable)
//   no valid temperature  -> kQualityUncompensated + HEALTH_DEPENDENCY_INVALID
//   !calibration.valid    -> HEALTH_UNCALIBRATED (nominal datasheet curve, D13)
//   otherwise             -> HEALTH_OK
// poll() returns READY for all of these; the health carries the verdict.
//
// Not detectable here: 0 V is distilled water, a probe in air, an open lead or
// a dead rail alike, so there is no UNPOWERED threshold. Rail state is left to
// acquisition (rail epoch, WP9).
//
// Worst-case blocking (one ADC conversion C, well under 1 ms):
//   init()    1 C   (throwaway conversion, checks the channel)
//   start()   1 C   (first sample of the window)
//   poll()    1 C   Pending until the window is full (>= 560 ms)
//   others    none
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/adc_channel.h"
#include "domain/calibration.h"

#define TDS_SAMPLES 15
#define TDS_TRIM 5
#define TDS_SAMPLE_SPACING_MS 40

typedef struct TdsConfig {
  AdcChannel* adc;                // already begun on BOARD_PIN_TDS_ADC
  LinearCalibration calibration;  // K-factor: x = nominal ppm, y = ppm
} TdsConfig;

// Members after `base` are private to tds.c.
typedef struct TdsDriver {
  SensorDriver base;  // must stay first
  TdsConfig config_;
  bool initialised_;
  bool busy_;
  bool saturated_;
  uint8_t count_;
  uint32_t samples_[TDS_SAMPLES];  // pin mV
  MonoMs startMs_;
  MonoMs lastSampleMs_;  // next tap is due TDS_SAMPLE_SPACING_MS after this
  Reading waterTemperature_;
  Reading result_;
} TdsDriver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void tdsDriverConstruct(TdsDriver* self, const TdsConfig* config);

// Compensation input from the latest DS18B20 reading (copied, not shared).
// Checked for health and age when the window finishes; no ADC access here.
void tdsDriverSetWaterTemperature(TdsDriver* self, const Reading* waterTemperature);

// New K-factor from the applied configuration (on-device calibration, WP8).
// Called by the acquisition owner between windows; applies from the next one.
void tdsDriverSetCalibration(TdsDriver* self, const LinearCalibration* calibration);

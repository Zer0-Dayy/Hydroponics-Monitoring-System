// dfr0026_light driver. Work package WP3 (Stage 2). Architecture §3, §14.
// Source: DFR0026.PDF (DFRobot Analog Ambient Light Sensor V2, pages 1-4).
//
// Retained 3.3 V rail. S -> 10 kOhm series -> ADC1 pin (GPIO1), 1 uF to GND
// (tau ~10 ms). No divider: pin mV = module output mV. More light, higher
// voltage. Datasheet range 1-6000 lux, but the output has no lux unit: the FAQ
// requires a reference-meter mapping (D13). Without one every result is mV.
// With an on-device fit (Calibration page: one reference-meter reading, gain
// through zero) the value is lux. raw stays pin mV either way, and the lamp
// thresholds use it (lamp_control), so calibrating never moves the lamp.
//
// Filter: LIGHT_SAMPLES conversions spaced LIGHT_SAMPLE_SPACING_MS apart,
// sorted, mean of the middle LIGHT_SAMPLES - 2*LIGHT_TRIM. Mains lamps flicker
// at 100 Hz (120 Hz on 60 Hz grids) and the RC only cuts 100 Hz to ~16 %, so
// the spacing is deliberately NOT a multiple of 10 ms: 11 ms steps the flicker
// phase by 1 ms per tap and the window covers the whole period. The window is
// spread over successive poll() calls, never taken as a burst (§9): each tap is
// due LIGHT_SAMPLE_SPACING_MS after the previous actual sample.
//
// Results (poll() returns READY for all; raw = filtered pin mV):
//   calibration.valid -> value = lux, UNIT_LUX, HEALTH_OK
//   otherwise         -> value = module mV, UNIT_MILLIVOLT, HEALTH_UNCALIBRATED
//   any sample clipped -> kQualitySaturated, value kept as a lower bound
//                         (bright light on a 3.3 V supply can exceed the S3's
//                         ~3.1 V range; this is not a fault)
//   ADC error          -> HEALTH_FAULT
// No no-signal threshold: ~0 mV is real darkness. With 1 uF on the pin and no
// pull-down, an unplugged S line holds its last level while the capacitor leaks;
// the driver cannot tell that apart. Characterise it at bring-up.
//
// Worst-case blocking (one ADC conversion C, well under 1 ms):
//   init()    1 C   (throwaway conversion, checks the channel)
//   start()   1 C   (first sample of the window)
//   poll()    1 C   Pending until the window is full (>= 99 ms)
//   others    none
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/adc_channel.h"
#include "domain/calibration.h"

#define LIGHT_SAMPLES 10
#define LIGHT_TRIM 2
#define LIGHT_SAMPLE_SPACING_MS 11

typedef struct LightConfig {
  AdcChannel* adc;                // already begun on BOARD_PIN_LIGHT_ADC
  LinearCalibration calibration;  // x = pin mV, y = lux; invalid = publish mV
} LightConfig;

// Members after `base` are private to light.c.
typedef struct LightDriver {
  SensorDriver base;  // must stay first
  LightConfig config_;
  bool initialised_;
  bool busy_;
  bool saturated_;
  uint8_t count_;
  uint32_t samples_[LIGHT_SAMPLES];  // pin mV
  MonoMs startMs_;
  MonoMs lastSampleMs_;  // next tap is due LIGHT_SAMPLE_SPACING_MS after this
  Reading result_;
} LightDriver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void lightDriverConstruct(LightDriver* self, const LightConfig* config);

// New lux fit from the applied configuration (on-device calibration, WP8).
// Called by the acquisition owner between windows; applies from the next one.
void lightDriverSetCalibration(LightDriver* self, const LinearCalibration* calibration);

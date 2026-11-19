#include "hal/adc_channel.h"

#include <Arduino.h>
#include <stddef.h>

// ESP32-S3: ADC1 is GPIO1-10 (ADC2 conflicts with Wi-Fi, §3).
static const int8_t kAdc1FirstPin = 1;
static const int8_t kAdc1LastPin = 10;

// At 11 dB (ADC_ATTEN_DB_12 in IDF 5) the S3 measures about 0-3100 mV. Treat a
// calibrated result at or above this ceiling as clipped. Characterise per board.
static const uint32_t kSaturationMv = 3050;

bool adcChannelBegin(AdcChannel* ch, int8_t pin) {
  ch->pin_ = ADC_CHANNEL_NO_PIN;
  if (pin < kAdc1FirstPin || pin > kAdc1LastPin) return false;
  analogReadResolution(12);
  analogSetPinAttenuation((uint8_t)pin, ADC_11db);
  ch->pin_ = pin;
  return true;
}

bool adcChannelReadMilliVolts(AdcChannel* ch, uint32_t* mv, bool* saturated) {
  if (mv) *mv = 0;
  if (saturated) *saturated = false;
  if (ch->pin_ < 0) return false;
  // eFuse-calibrated conversion (esp_adc_cal). One conversion, a few tens of us.
  const uint32_t value = analogReadMilliVolts((uint8_t)ch->pin_);
  if (mv) *mv = value;
  if (saturated) *saturated = value >= kSaturationMv;
  return true;
}

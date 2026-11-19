// ADC1 channel adapter (HAL). Work package WP3 (shared by light, pH and TDS).
// ADC1 only: ADC2 conflicts with Wi-Fi (§3). Returns calibrated millivolts at
// the ESP32 pin; divider recovery and units belong to domain/calibration.h.
//
// 12-bit, 11 dB attenuation (S3 usable range about 0-3100 mV), eFuse calibration.
// Worst-case blocking: one conversion per read, well under 1 ms.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ADC_CHANNEL_NO_PIN ((int8_t)-1)

// Treat the members as private to adc_channel.c. Initialise with ADC_CHANNEL_INIT.
typedef struct AdcChannel {
  int8_t pin_;
} AdcChannel;

#define ADC_CHANNEL_INIT {ADC_CHANNEL_NO_PIN}

// false if the pin is not an ADC1 pin (GPIO1-10 on the S3); the channel then
// stays unusable and every read fails.
bool adcChannelBegin(AdcChannel* ch, int8_t pin);
// One conversion. false on error. Reports saturation near full scale.
bool adcChannelReadMilliVolts(AdcChannel* ch, uint32_t* mv, bool* saturated);

// SGP30 word framing and humidity conversion (WP6). Pure C, host-tested.
// Datasheet: Sensirion SGP30 v1.0, §6.3 (humidity compensation), §6.6 (CRC).
//
// Every 16-bit word on the wire is MSB, LSB, CRC-8. The CRC is mandatory in
// the write direction; the driver also checks it on every word it reads.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// CRC-8: polynomial 0x31, init 0xFF, no reflection, no final XOR.
// Datasheet test vector: CRC(0xBE, 0xEF) = 0x92.
uint8_t sgp30Crc8(const uint8_t* data, size_t len);

// out = {MSB, LSB, CRC}.
void sgp30PackWord(uint16_t word, uint8_t out[3]);

// Decode nWords 3-byte groups from buf into out[]. false on any CRC mismatch
// (out is then not fully written). An all-0xFF floating-bus read fails here.
bool sgp30UnpackWords(const uint8_t* buf, uint8_t nWords, uint16_t* out);

// g/m3 as the sensor's 8.8 fixed-point value, rounded and saturated to
// 0x0001..0xFFFF. 0 for a non-positive or NaN input (0 disables compensation).
uint16_t sgp30GramsToFixed8p8(float gramsPerM3);

// Absolute humidity (g/m3) from air temperature and relative humidity, using
// the datasheet's Magnus formula. Returns a negative value for invalid input.
float sgp30AbsoluteHumidity(float tempC, float rhPercent);

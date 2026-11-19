// DS18B20 scratchpad decoding and 1-Wire CRC (WP2). Pure C, host-tested.
// Datasheet: Maxim DS18B20 (local copy in Electrical System Study/Datasheets/
// DS18B20.pdf): Figure 9 (memory map), Figure 10 (configuration register),
// Table 1 (temperature format), "CRC Generation".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS18B20_FAMILY_CODE 0x28
#define DS18B20_SCRATCHPAD_LEN 9
#define DS18B20_POWER_ON_RAW ((int16_t)0x0550)  // +85 degC, reset value of the register
#define DS18B20_CONFIG_12BIT 0x7F               // R1 = R0 = 1, reserved bits as read
#define DS18B20_MIN_C (-55.0f)
#define DS18B20_MAX_C 125.0f

// Dallas/Maxim CRC-8 (x^8 + x^5 + x^4 + 1), LSB first, init 0. Running it over
// data plus its CRC byte gives 0.
uint8_t oneWireCrc8(const uint8_t* data, size_t len);

// ROM code: CRC over the first 7 bytes matches byte 7 and the family is 28h.
// Rejects the all-zero code a bus stuck low produces.
bool ds18b20RomValid(const uint8_t rom[8]);

// Scratchpad CRC matches AND the configuration register has its fixed bits
// (bit 7 = 0, bits 0-4 = 1). The second check rejects an all-zero read (bus
// stuck low), whose CRC is also 0.
bool ds18b20ScratchpadValid(const uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]);

// True if the configuration register selects 12-bit resolution.
bool ds18b20Is12Bit(uint8_t config);

// Signed temperature register (bytes 0-1), 1/16 degC per LSB at 12 bits.
int16_t ds18b20RawTemperature(const uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]);
float ds18b20RawToDegC(int16_t raw);

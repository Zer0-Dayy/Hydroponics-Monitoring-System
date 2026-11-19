#include "domain/ds18b20_codec.h"

uint8_t oneWireCrc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    uint8_t in = data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      const uint8_t mix = (crc ^ in) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;  // reflected 0x31
      in >>= 1;
    }
  }
  return crc;
}

bool ds18b20RomValid(const uint8_t rom[8]) {
  return rom[0] == DS18B20_FAMILY_CODE && oneWireCrc8(rom, 7) == rom[7];
}

bool ds18b20ScratchpadValid(const uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]) {
  if (oneWireCrc8(scratchpad, 8) != scratchpad[8]) return false;
  return (scratchpad[4] & 0x9F) == 0x1F;
}

bool ds18b20Is12Bit(uint8_t config) { return (config & 0x60) == 0x60; }

int16_t ds18b20RawTemperature(const uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]) {
  return (int16_t)(uint16_t)((uint16_t)scratchpad[1] << 8 | scratchpad[0]);
}

float ds18b20RawToDegC(int16_t raw) { return (float)raw / 16.0f; }

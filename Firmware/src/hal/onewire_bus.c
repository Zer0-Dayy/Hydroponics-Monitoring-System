#include "hal/onewire_bus.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <string.h>

#include "domain/ds18b20_codec.h"

// ESP32-S3 GPIO numbers run 0-48.
static const int8_t kMaxGpio = 48;

static const uint8_t kCmdSearchRom = 0xF0;
static const uint8_t kCmdMatchRom = 0x55;

// Slot timing (us), DS18B20 datasheet AC characteristics: tSLOT 60-120,
// tLOW1 1-15, tLOW0 60-120, tRDV 15, tREC >= 1, tRSTL/tRSTH >= 480,
// presence low starts 15-60 us after release and lasts 60-240 us.
enum {
  kResetLowUs = 480,
  kPresenceSampleUs = 70,
  kResetHighRestUs = 410,  // 70 + 410 = tRSTH
  kWrite1LowUs = 6,
  kWrite1RestUs = 64,
  kWrite0LowUs = 60,
  kWrite0RestUs = 10,
  kReadLowUs = 3,
  kReadSampleUs = 10,  // sampled 13 us into the slot, inside tRDV
  kReadRestUs = 53
};

// Interrupts off for one slot only (bit timing must not be stretched).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void busLow(const OneWireBus* bus) { gpio_set_level((gpio_num_t)bus->pin_, 0); }
static inline void busRelease(const OneWireBus* bus) { gpio_set_level((gpio_num_t)bus->pin_, 1); }
static inline bool busLevel(const OneWireBus* bus) {
  return gpio_get_level((gpio_num_t)bus->pin_) != 0;
}

bool oneWireBusBegin(OneWireBus* bus, int8_t pin) {
  bus->pin_ = -1;
  if (pin < 0 || pin > kMaxGpio) return false;
  gpio_config_t io;
  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = 1ULL << (uint8_t)pin;
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;  // drive low or release; read back the line
  io.pull_up_en = GPIO_PULLUP_DISABLE;  // external 4.7 kOhm to 3.3 V
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&io) != ESP_OK) return false;
  bus->pin_ = pin;
  busRelease(bus);
  return true;
}

bool oneWireBusReset(OneWireBus* bus) {
  if (bus->pin_ < 0) return false;
  busRelease(bus);
  if (!busLevel(bus)) return false;  // stuck low: short or no pull-up
  busLow(bus);
  delayMicroseconds(kResetLowUs);
  portENTER_CRITICAL(&s_mux);
  busRelease(bus);
  delayMicroseconds(kPresenceSampleUs);
  const bool presence = !busLevel(bus);
  portEXIT_CRITICAL(&s_mux);
  delayMicroseconds(kResetHighRestUs);
  return presence;
}

static void writeBit(OneWireBus* bus, bool bit) {
  portENTER_CRITICAL(&s_mux);
  busLow(bus);
  delayMicroseconds(bit ? kWrite1LowUs : kWrite0LowUs);
  busRelease(bus);
  portEXIT_CRITICAL(&s_mux);
  delayMicroseconds(bit ? kWrite1RestUs : kWrite0RestUs);
}

static bool readBit(OneWireBus* bus) {
  portENTER_CRITICAL(&s_mux);
  busLow(bus);
  delayMicroseconds(kReadLowUs);
  busRelease(bus);
  delayMicroseconds(kReadSampleUs);
  const bool bit = busLevel(bus);
  portEXIT_CRITICAL(&s_mux);
  delayMicroseconds(kReadRestUs);
  return bit;
}

void oneWireBusWriteByte(OneWireBus* bus, uint8_t value) {
  if (bus->pin_ < 0) return;
  for (uint8_t i = 0; i < 8; ++i) writeBit(bus, (value >> i) & 1u);  // LSB first
}

uint8_t oneWireBusReadByte(OneWireBus* bus) {
  if (bus->pin_ < 0) return 0xFF;
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (readBit(bus)) value |= (uint8_t)(1u << i);
  }
  return value;
}

bool oneWireBusSearchFirst(OneWireBus* bus, uint8_t rom[8]) {
  if (!oneWireBusReset(bus)) return false;
  oneWireBusWriteByte(bus, kCmdSearchRom);
  uint8_t id[8] = {0};
  for (uint8_t i = 0; i < 64; ++i) {
    const bool bit = readBit(bus);
    const bool complement = readBit(bus);
    if (bit && complement) return false;  // no device took part
    // Different: every device has `bit` here. Equal (both 0): devices differ,
    // take the 0 branch, which is the first device in search order.
    const bool direction = bit != complement ? bit : false;
    if (direction) id[i / 8] |= (uint8_t)(1u << (i % 8));
    writeBit(bus, direction);
  }
  if (oneWireCrc8(id, 7) != id[7]) return false;
  memcpy(rom, id, 8);
  return true;
}

void oneWireBusSelect(OneWireBus* bus, const uint8_t rom[8]) {
  oneWireBusWriteByte(bus, kCmdMatchRom);
  for (uint8_t i = 0; i < 8; ++i) oneWireBusWriteByte(bus, rom[i]);
}

uint8_t oneWireBusCrc8(const uint8_t* data, size_t len) { return oneWireCrc8(data, len); }

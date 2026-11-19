#include "hal/i2c_bus.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp32-hal-i2c.h>

// Controller 0 (what the C++ version reached through the Arduino `Wire`
// object). Calls go straight to the core's C I2C layer, with the same
// per-transaction timeout Wire.setTimeOut() used to apply.
static const uint8_t kI2cPort = 0;

bool i2cBusBegin(I2cBus* bus, int8_t sda, int8_t scl, uint32_t clockHz) {
  if (bus->started_) i2cBusEnd(bus);
  bus->sda_ = sda;
  bus->scl_ = scl;
  bus->clockHz_ = clockHz;
  // As Wire.begin(): a controller that is already running counts as started.
  bus->started_ = i2cIsInit(kI2cPort) || i2cInit(kI2cPort, sda, scl, clockHz) == ESP_OK;
  return bus->started_;
}

bool i2cBusProbe(I2cBus* bus, uint8_t address) {
  if (!bus->started_) return false;
  // Address-only write: ACK => present.
  return i2cWrite(kI2cPort, address, NULL, 0, I2C_BUS_TIMEOUT_MS) == ESP_OK;
}

bool i2cBusWriteReg(I2cBus* bus, uint8_t address, uint8_t reg, uint8_t value) {
  if (!bus->started_) return false;
  const uint8_t tx[2] = {reg, value};
  return i2cWrite(kI2cPort, address, tx, sizeof(tx), I2C_BUS_TIMEOUT_MS) == ESP_OK;
}

bool i2cBusReadRegs(I2cBus* bus, uint8_t address, uint8_t reg, uint8_t* buf, size_t len) {
  if (!bus->started_ || buf == NULL || len == 0) return false;
  // Register write, repeated start, read: one bounded transaction.
  size_t got = 0;
  i2cWriteReadNonStop(kI2cPort, address, &reg, 1, buf, len, I2C_BUS_TIMEOUT_MS, &got);
  return got == len;  // a partial or failed read is dropped
}

bool i2cBusWrite(I2cBus* bus, uint8_t address, const uint8_t* tx, size_t len) {
  if (!bus->started_ || tx == NULL || len == 0) return false;
  return i2cWrite(kI2cPort, address, tx, len, I2C_BUS_TIMEOUT_MS) == ESP_OK;
}

bool i2cBusRead(I2cBus* bus, uint8_t address, uint8_t* buf, size_t len) {
  if (!bus->started_ || buf == NULL || len == 0) return false;
  size_t got = 0;
  i2cRead(kI2cPort, address, buf, len, I2C_BUS_TIMEOUT_MS, &got);
  return got == len;  // a partial or failed read is dropped
}

// Release a slave holding SDA low: up to 9 SCL pulses, then a STOP.
// Bounded (~0.2 ms); no retry loop.
bool i2cBusRecover(I2cBus* bus) {
  const int8_t sda = bus->sda_;
  const int8_t scl = bus->scl_;
  if (sda < 0 || scl < 0) return false;
  // An idle bus has SDA high: the failure was a NACK (e.g. a device that is
  // absent or busy), not a stuck bus. Leave the controller running so the
  // other device on the bus is not disturbed (audit L5).
  if (bus->started_ && gpio_get_level((gpio_num_t)sda) != 0) return true;
  if (bus->started_) i2cBusEnd(bus);

  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  for (uint8_t i = 0; i < 9 && digitalRead(sda) == LOW; ++i) {
    digitalWrite(scl, LOW);
    delayMicroseconds(5);
    digitalWrite(scl, HIGH);
    delayMicroseconds(5);
  }

  // STOP: SDA low -> SCL high -> SDA high.
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, LOW);
  digitalWrite(sda, LOW);
  delayMicroseconds(5);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, HIGH);
  delayMicroseconds(5);

  pinMode(sda, INPUT_PULLUP);
  const bool sdaReleased = digitalRead(sda) == HIGH;
  return i2cBusBegin(bus, sda, scl, bus->clockHz_) && sdaReleased;
}

void i2cBusEnd(I2cBus* bus) {
  if (bus->started_ && i2cIsInit(kI2cPort)) i2cDeinit(kI2cPort);
  bus->started_ = false;
}

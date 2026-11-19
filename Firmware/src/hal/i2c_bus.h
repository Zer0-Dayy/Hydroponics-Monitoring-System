// I2C bus adapter (HAL). Work package WP1 (with BME280).
// Single owner: only the acquisition lease holder calls it (§9).
// Every transaction must have a bounded timeout; i2cBusRecover() performs a
// limited stuck-bus recovery (§16), never a blocking retry loop.
//
// Worst-case blocking: I2C_BUS_TIMEOUT_MS per transaction (probe/writeReg/
// readRegs/write/read); recover() ~0.2 ms of bit-banging plus one begin().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define I2C_BUS_TIMEOUT_MS ((uint16_t)5)

// Treat the members as private to i2c_bus.c. Initialise with I2C_BUS_INIT.
typedef struct I2cBus {
  bool started_;
  int8_t sda_;
  int8_t scl_;
  uint32_t clockHz_;
} I2cBus;

#define I2C_BUS_INIT {false, -1, -1, 0}

bool i2cBusBegin(I2cBus* bus, int8_t sda, int8_t scl, uint32_t clockHz);
bool i2cBusProbe(I2cBus* bus, uint8_t address);
bool i2cBusWriteReg(I2cBus* bus, uint8_t address, uint8_t reg, uint8_t value);
bool i2cBusReadRegs(I2cBus* bus, uint8_t address, uint8_t reg, uint8_t* buf, size_t len);
// Raw transfers for command-based devices (SGP30): each is one transaction
// ending in STOP, so a device that NACKs while busy is never held by a
// repeated start. A read returns true only if all `len` bytes arrived.
bool i2cBusWrite(I2cBus* bus, uint8_t address, const uint8_t* tx, size_t len);
bool i2cBusRead(I2cBus* bus, uint8_t address, uint8_t* buf, size_t len);
// Call after a failed transaction. Does nothing (returns true) when SDA is
// high, i.e. the failure was only a NACK; recovers only a bus held low.
bool i2cBusRecover(I2cBus* bus);
void i2cBusEnd(I2cBus* bus);

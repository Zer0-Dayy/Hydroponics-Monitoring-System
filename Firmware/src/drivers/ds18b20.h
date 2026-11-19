// ds18b20 driver. Work package WP2 (Stage 2). Architecture §7, §14.
// Datasheet: Maxim DS18B20 (local copy in Electrical System Study/Datasheets/
// DS18B20.pdf). Domain math (CRC, decoding) in domain/ds18b20_codec.h.
//
// Retained 3.3 V supply (external power, no parasite mode), 1-Wire on GPIO14
// with a 4.7 kOhm pull-up. Produces WaterTemperature in degC.
//
// init() finds the first device with Search ROM, checks the ROM CRC and the
// family code (28h) and keeps that ROM: every later transaction uses Match
// ROM, so a second device on the bus is never read by mistake. It then reads
// the scratchpad and, if the EEPROM selected a lower resolution, writes 12-bit
// to the scratchpad only (no Copy Scratchpad: no EEPROM wear, re-applied on
// every init).
// start() sends Convert T and returns; poll() is Pending until
// CFG_DS18B20_CONVERSION_MAX_MS (750 ms, 12-bit tCONV max) has passed, then
// reads the 9-byte scratchpad once.
//
// Results, highest priority first (poll() returns READY for all of them):
//   scratchpad CRC or fixed config bits wrong -> HEALTH_FAULT, no raw
//   temperature register = 0550h (+85 degC power-on value) -> HEALTH_FAULT;
//                     the device reset without converting, so the next start()
//                     fails and init() re-applies the resolution
//   outside -55..+125 degC -> HEALTH_FAULT
//   otherwise         -> value in degC, HEALTH_OK
// raw = temperature register (1/16 degC) whenever the CRC is good.
// No presence pulse in start()/poll() -> DRIVER_NOT_PRESENT (re-init needed).
// Before the first result readResult() gives HEALTH_STARTING, never 0 degC.
//
// Worst-case blocking (1-Wire HAL; slots run with interrupts off <= 70 us each):
//   init()    ~50 ms  (search ~15 ms, scratchpad read ~12 ms, and when the
//                      resolution must be set: write ~9 ms + re-read ~12 ms)
//   start()   ~7 ms   (reset, Match ROM, Convert T)
//   poll()    ~12 ms  once the conversion time is over (reset, Match ROM,
//                      Read Scratchpad, 9 bytes); Pending polls: none
//   others    none
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/onewire_bus.h"

typedef struct Ds18b20Config {
  OneWireBus* bus;  // already begun on BOARD_PIN_ONE_WIRE
} Ds18b20Config;

// Members after `base` are private to ds18b20.c.
typedef struct Ds18b20Driver {
  SensorDriver base;  // must stay first
  Ds18b20Config config_;
  bool initialised_;
  bool busy_;
  bool hasRom_;
  uint8_t rom_[8];
  MonoMs startMs_;
  Reading result_;
} Ds18b20Driver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void ds18b20DriverConstruct(Ds18b20Driver* self, const Ds18b20Config* config);

// ROM code selected by the last successful init(). false before that.
bool ds18b20DriverGetRom(const Ds18b20Driver* self, uint8_t rom[8]);

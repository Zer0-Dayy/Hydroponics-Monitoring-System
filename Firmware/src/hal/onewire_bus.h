// 1-Wire bus adapter (HAL). Work package WP2 (with DS18B20).
// Externally powered DS18B20 on BOARD_PIN_ONE_WIRE (4.7 kOhm pull-up).
//
// Bit-banged, open-drain GPIO (the pin only pulls low; the pull-up drives
// high). Standard-speed timing from the DS18B20 datasheet (slot 70 us).
// Each time slot runs with interrupts off on this core for at most ~70 us;
// the reset's 480 us low phase runs with interrupts on.
// Worst-case blocking: reset ~1 ms, one byte ~0.6 ms, search ~15 ms.
// No parasite power: there is no strong pull-up.
// WP7: do not run a 1-Wire transaction on the core that services the HC-SR04
// echo ISR while an echo is in flight - a masked edge is timestamped late
// (up to ~70 us, ~1 cm).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Treat the members as private to onewire_bus.c. Initialise with ONE_WIRE_BUS_INIT.
typedef struct OneWireBus {
  int8_t pin_;
} OneWireBus;

#define ONE_WIRE_BUS_INIT {-1}

// false if the pin is not a usable GPIO; the bus then stays unusable.
bool oneWireBusBegin(OneWireBus* bus, int8_t pin);
// true = presence pulse detected. false also if the bus does not idle high
// (shorted to GND or missing pull-up).
bool oneWireBusReset(OneWireBus* bus);
void oneWireBusWriteByte(OneWireBus* bus, uint8_t value);
uint8_t oneWireBusReadByte(OneWireBus* bus);
// Search ROM [F0h] for the first device (lowest branch at each discrepancy).
// Does its own reset. true if a code with a valid CRC was read into rom.
bool oneWireBusSearchFirst(OneWireBus* bus, uint8_t rom[8]);
// Match ROM [55h] + code. Call right after a successful reset.
void oneWireBusSelect(OneWireBus* bus, const uint8_t rom[8]);
uint8_t oneWireBusCrc8(const uint8_t* data, size_t len);

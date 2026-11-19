// Board profile: pins, rails and divider values for the ESP32-S3-WROOM-1 N16R8
// board (decision D16, supersedes D02).
// Source: Firmware Architecture Rev 3 §5 (Sensor <-> ESP32-S3 Signal Pinout,
// 22 Oct 2026, + Power Circuit Diagram). FROZEN CONTRACT - change only with a
// hardware-verified reason.
//
// Pure C (no Arduino headers) so domain code and native tests can use it.
// Anything not on the drawing stays BOARD_PIN_UNASSIGNED, and a pin on the
// drawing whose behaviour is not yet measured keeps its feature disabled
// (see boardProfileValidate()).
// Reserved on the N16R8: GPIO35-37 (octal PSRAM), GPIO19/20 (native USB),
// strapping pins GPIO0/3/45/46.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BOARD_PIN_UNASSIGNED ((int8_t)-1)

typedef uint8_t Rail;
enum {
  RAIL_3V3 = 0,         // retained 3.3 V supply, kept on battery
  RAIL_5V_SWITCHED = 1  // external 5 V branches, lost on battery
};

// Bit mask used by the acquisition lease (allowed rails for a worker).
#define BOARD_RAIL_MASK_3V3 ((uint8_t)(1u << 0))
#define BOARD_RAIL_MASK_5V ((uint8_t)(1u << 1))

// ---- Shared I2C bus (retained 3.3 V; 4.7 kOhm pull-ups to 3.3 V) ------------
#define BOARD_PIN_I2C_SDA ((int8_t)21)
#define BOARD_PIN_I2C_SCL ((int8_t)47)
#define BOARD_I2C_CLOCK_HZ ((uint32_t)400000)

// ---- BME280 (retained 3.3 V, I2C) ------------------------------------------
#define BOARD_BME280_I2C_ADDRESS ((uint8_t)0x76)      // SDO = GND
#define BOARD_BME280_I2C_ADDRESS_ALT ((uint8_t)0x77)  // SDO = VDDIO

// ---- SGP30 module (retained 3.3 V, I2C, TVOC / CO2eq) -----------------------
#define BOARD_SGP30_I2C_ADDRESS ((uint8_t)0x58)  // fixed

// ---- DS18B20 (retained 3.3 V, 1-Wire, external supply, 4.7 kOhm pull-up) ----
#define BOARD_PIN_ONE_WIRE ((int8_t)14)

// ---- DFR0026 ambient light (retained 3.3 V, ADC1_CH0) -----------------------
// S -> 10 kOhm series -> GPIO1, 1 uF to GND (tau ~10 ms).
#define BOARD_PIN_LIGHT_ADC ((int8_t)1)

// ---- SEN0244 TDS (filtered external 5 V, ADC1_CH5, 0-2.3 V nominal) ---------
// A -> GPIO6, 100 nF to GND.
#define BOARD_PIN_TDS_ADC ((int8_t)6)

// ---- pH-4502C (filtered external 5 V, ADC1_CH6, divider 10k top / 15k bottom)
// PO -> 10k/15k -> GPIO7 + 1 uF to GND. DO and TO not connected.
#define BOARD_PIN_PH_ADC ((int8_t)7)
#define BOARD_PH_DIVIDER_TOP_OHM ((uint32_t)10000)
#define BOARD_PH_DIVIDER_BOTTOM_OHM ((uint32_t)15000)

// ---- HC-SR04 (external 5 V, ECHO through 1k/2k divider) ---------------------
#define BOARD_PIN_HCSR04_TRIG ((int8_t)17)
#define BOARD_PIN_HCSR04_ECHO ((int8_t)18)

// ---- External-power sense (ADC1_CH7; 220 Ohm over 2 x 220 Ohm from ext. 5 V) -
// ~3.33 V at 5.00 V. Used as a digital edge input; the ADC can check levels.
#define BOARD_PIN_EXTERNAL_POWER_SENSE ((int8_t)8)
#define BOARD_POWER_SENSE_DIVIDER_TOP_OHM ((uint32_t)220)
#define BOARD_POWER_SENSE_DIVIDER_BOTTOM_OHM ((uint32_t)440)
// Measured 25 Oct 2026 with test_power_sense (breadboard, 6 losses / 5 power-ons,
// README "External-power sense bring-up"): LOW 0-53 mV, HIGH >= 3.05 V (ADC
// clipped; <= 3.6 V by calculation only, 3.17-3.50 V for a 4.75-5.25 V supply).
// Switches at ~1.65 V falling / ~1.95 V rising. Power loss chatters 479-928
// edges over 5-14 ms; power-on 3-16 edges over < 0.2 ms. First falling edge
// 59-220 ms after the rail starts to drop (+ rail already ~2.5 V).
// Requirements on WP9: the ISR disables itself on the first edge and the power
// task re-reads the level after BOARD_POWER_SENSE_QUALIFY_MS before acting and
// re-arming; the rail epoch also rejects 5 V readings that ended just before it.
#define BOARD_POWER_SENSE_VERIFIED true
#define BOARD_POWER_SENSE_QUALIFY_MS ((uint32_t)20)

// ---- Relay 1 (SRD-05VDC, coil on external 5 V) -------------------------------
// GPIO15 -> 220 Ohm -> IRLZ44N gate (low side), 10 kOhm gate pull-down,
// 1N5822 flyback. High = coil energised. Contact use and load still open (§12).
#define BOARD_PIN_RELAY1 ((int8_t)15)
#define BOARD_RELAY1_ACTIVE_LOW false

// ---- Open hardware gates (§20) - NOT on the drawing ---------------------------
#define BOARD_PIN_RAIL_5V_FEEDBACK BOARD_PIN_UNASSIGNED

static inline bool boardPinAssigned(int8_t pin) { return pin != BOARD_PIN_UNASSIGNED; }

typedef struct BoardValidation {
  bool powerSenseAssigned;
  bool railFeedbackAssigned;
  bool relayAssigned;
  bool productionReady;  // false => report a non-production build status
} BoardValidation;

static inline BoardValidation boardProfileValidate(void) {
  BoardValidation v;
  v.powerSenseAssigned = boardPinAssigned(BOARD_PIN_EXTERNAL_POWER_SENSE);
  v.railFeedbackAssigned = boardPinAssigned(BOARD_PIN_RAIL_5V_FEEDBACK);
  v.relayAssigned = boardPinAssigned(BOARD_PIN_RELAY1);
  // Minimum gate for power modes: the sense pin exists AND has been measured.
  v.productionReady = v.powerSenseAssigned && BOARD_POWER_SENSE_VERIFIED;
  return v;
}

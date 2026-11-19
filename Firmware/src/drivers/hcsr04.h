// hcsr04 driver. Work package WP4 (Stage 3). Architecture §7, §12, §14.
// Source: HC-SR04.pdf (module guide, pages 2-5).
//
// Switched 5 V rail. TRIG (GPIO17) driven at 3.3 V logic; ECHO (GPIO18)
// through a 1 kOhm / 2 kOhm divider (5 V -> ~3.33 V). A 10 us trigger starts an
// 8-cycle 40 kHz burst; ECHO stays high for the round trip. Distance =
// width * 343 m/s / 2 (domain/ultrasonic.h), no temperature compensation.
// Datasheet range 2-400 cm, resolution 0.3 cm. One ping per frame (1 s) leaves
// old echoes time to die out (datasheet §5: echoes give false short readings).
//
// Results (poll() returns READY for all of them; the verdict is in health,
// raw = echo width or elapsed time in us). A missing distance is never 0:
//   echo 20-4000 mm          -> value mm, UNIT_MILLIMETRE, HEALTH_OK
//   echo outside 20-4000 mm  -> HEALTH_FAULT, raw = width
//   no rising edge by timeout -> HEALTH_TIMEOUT, no raw (stuck low, ECHO
//                               unplugged or 5 V rail off: indistinguishable,
//                               there is no rail feedback pin)
//   rose, no fall by timeout -> HEALTH_TIMEOUT, raw = elapsed (no object in
//                               range; clones hold ECHO ~38 ms or longer)
// ECHO already high at init()/start() (stuck high, or a late echo still
// running) -> DRIVER_FAULT, stored reading HEALTH_FAULT, re-init required.
// The timeout is Hcsr04Config.timeoutMs (CFG_HCSR04_TIMEOUT_MS = 30 ms covers
// the 23.3 ms round trip at 4 m plus the ~0.5 ms burst lead-in).
//
// Worst-case blocking:
//   init()            none (one GPIO read)
//   start()           ~12 us busy-wait (2 us TRIG low + 10 us TRIG high)
//   poll()            none (reads the ISR's timestamps); Pending, 1 ms steps,
//                     until the echo ends or timeoutMs
//   cancel(), preparePowerOff()  a few GPIO writes
// The ECHO edge ISR (hal/pulse_capture.c) only stores a timestamp.
//
// preparePowerOff(): TRIG low, ECHO input without pulls (no driving into the
// unpowered module).
//
// Implements the SensorDriver contract in drivers/sensor_driver.h.
#pragma once
#include "drivers/sensor_driver.h"
#include "hal/pulse_capture.h"

typedef struct Hcsr04Config {
  PulseCapture* capture;  // already begun on BOARD_PIN_HCSR04_TRIG / _ECHO
  uint32_t timeoutMs;     // CFG_HCSR04_TIMEOUT_MS
} Hcsr04Config;

// Members after `base` are private to hcsr04.c.
typedef struct Hcsr04Driver {
  SensorDriver base;  // must stay first
  Hcsr04Config config_;
  bool initialised_;
  bool busy_;
  MonoMs startMs_;
  Reading result_;
} Hcsr04Driver;

// Constructor. Use &driver.base wherever a SensorDriver* is expected.
void hcsr04DriverConstruct(Hcsr04Driver* self, const Hcsr04Config* config);

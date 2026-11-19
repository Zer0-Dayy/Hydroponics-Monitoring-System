# Hydroponics firmware (ESP32-S3-WROOM-1 N16R8)

> **Target (October 2026):** ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal
> PSRAM), decision **D16** (supersedes D02). `platformio.ini` uses
> `esp32-s3-devkitc-1`, and the env names are unchanged. The MQ-7 has been removed
> from the hardware and replaced by an **SGP30** on the retained 3.3 V supply (D18).

> **Status (28 Oct 2026):** WP1-WP8 done; WP8 8.0-8.4 hardware-tested, then
> the LAN link removed (Bluetooth is the only app link), on-device calibration
> added, and WP9 low-power mode written. The calibration and WP9 code is
> **host-tested and builds but has not been flashed yet** (no circuit), and WP9
> transitions ship **off** (`POLICY_POWER_TRANSITIONS_ENABLED 0`) until then. See
> "Connecting the app", "On-device calibration" and "Low-power mode (WP9)".

## Connecting the app

The [desktop app guide](../BluetoothControlApp/Readme.md) covers running from
source and building Windows and Linux packages. The app uses
[contract v2](../BluetoothControlApp/FirmwareContract.md) over Bluetooth LE.

1. Flash `esp32dev`. Boot logs `ble: advertising as Hydro-ESP1 (open link, no pairing)`.
2. In the app: **Connection** -> **Scan for controllers** -> **Connect**. There is no
   passkey and no pairing (firmware 0.11.0+).
3. Optional: **Wi-Fi** page -> SSID + password (sent over the BLE link,
   stored only in the ESP's NVS). The app itself never connects over Wi-Fi.
4. **Calibration** page when a probe needs it (pH, TDS, light).

**One computer at a time, no owner.** Any computer can connect, one at a time.
Disconnect (or close the app) on one computer and the ESP shows up on the next
one's scan right away. There is no BOOT-button recovery and no reset needed.
- A computer that just vanished frees the ESP within ~4 s (requested supervision timeout).
- A connection that never subscribes is dropped after 20 s.
- A pairing Windows/BlueZ still holds from firmware 0.10 or older is removed by
  the app before it connects.

**Trade-off:** the link is not encrypted. Anyone in Bluetooth range can read the
data and send commands, and the Wi-Fi password travels in clear.

## Pin map (Pin Layout, 22 Oct 2026; architecture §5)

| Signal | GPIO | Notes |
|--------|------|-------|
| I2C SDA / SCL (BME280 0x76, SGP30 0x58) | 21 / 47 | 4.7 kOhm pull-ups to 3.3 V |
| DS18B20 DQ | 14 | 4.7 kOhm pull-up, external 3.3 V supply |
| DFR0026 light | 1 (ADC1_CH0) | 10 kOhm series, 1 uF to GND |
| SEN0244 TDS | 6 (ADC1_CH5) | 100 nF to GND, filtered external 5 V |
| pH-4502C PO | 7 (ADC1_CH6) | 10 kOhm / 15 kOhm divider, 1 uF; DO/TO not connected |
| HC-SR04 TRIG / ECHO | 17 / 18 | ECHO through 1 kOhm / 2 kOhm divider |
| External-power sense | 8 (ADC1_CH7) | 220 Ohm over 2 x 220 Ohm from external 5 V (~3.33 V) |
| Relay drive | 15 | IRLZ44N low side, 220 Ohm gate, 10 kOhm pull-down; active-high |
| Native USB D- / D+ | 19 / 20 | 22 Ohm series |

GPIO35-37 are used by the PSRAM; GPIO0, 3, 45 and 46 are strapping pins.

> `include/board_profile.h` matches this table (remapped 22 Oct 2026).
> `BOARD_POWER_SENSE_VERIFIED` is `true` since 25 Oct 2026 (GPIO8 measured with
> `test_power_sense`, see "External-power sense bring-up"); WP9 must apply
> `BOARD_POWER_SENSE_QUALIFY_MS`. The MQ-7 code is gone: `SENSOR_MQ7` and
> `METRIC_CO_SIGNAL` became `SENSOR_SGP30`, `METRIC_TVOC` and `METRIC_CO2EQ`
> (`Snapshot` 528 -> 584 bytes), and `drivers/mq7.*` became the `drivers/sgp30.*`
> stub with its own `test_sgp30` env.

New layered FreeRTOS firmware. The design authority is
[`Firmware Architecture.pdf`](Firmware%20Architecture.pdf), Rev 3 (22 Oct 2026). Its
editable source is [`docs/architecture/`](docs/architecture/); rebuild with
`python docs/architecture/build.py` (needs ReportLab and the Windows Segoe UI and
Consolas fonts). Section numbers (§) below
refer to Rev 3. The old team code is kept unchanged and is used only as reference.

The Stage 4 FreeRTOS design (tasks, scheduling, channels, Safety, supervision) is in
[`RTOS Architecture.pdf`](RTOS%20Architecture.pdf); its source is [`docs/rtos/`](docs/rtos/)
(`python docs/rtos/build.py`, reuses the kit in `docs/architecture/`).

**Status: implementation stages 1-6 written.** Every driver (WP1-WP6), acquisition,
Safety and supervision (WP7, WP10), the BLE link and configuration (WP8) and the
low-power machinery (WP9) are in the production image. What is still open is
hardware evidence, not code: see the status note at the top and "Work packages".

## Language: C (migrated from C++, October 2026)

All project sources (`src/`, `include/`, `test/`) are C, except
`src/transports/ble_gatt.cpp`: the C++ shim over NimBLE-Arduino, with a C API, from WP8. The module layout, layer
rules, frozen-contract contents and env names are the same as in the C++ version.
Runtime behaviour is meant to be unchanged, but it has not yet been re-verified on
hardware (see "C migration build record" below). The pre-migration C++ tree was
not kept; its build numbers are in that record. The framework is still
Arduino-ESP32 on the same pinned platform, so the SDK settings below still apply.
The framework is C++ inside, but project code only uses its C APIs (`esp32-hal-*`,
FreeRTOS, `esp_timer`).

The architecture records this as decision **D17** (Rev 3, §7 and §20).
`Stage 1 Report.html` still describes the C++ code.

How the C++ constructs map to C:

| C++ | C |
|-----|-----|
| `namespace cfg` / `board` constants | `CFG_*` / `BOARD_*` macros with the original types, e.g. `CFG_BME280_PERIOD_MS`, `BOARD_PIN_I2C_SDA` |
| `enum class X : uint8_t` | `typedef uint8_t X;` plus prefixed constants (`SENSOR_*`, `METRIC_*`, `UNIT_*`, `HEALTH_*`, `DRIVER_*`, `CAPTURE_*`, `POWER_MODE_*`, `TASK_ID_*`, `LOG_LEVEL_*`, ...). Records keep their C++ size. `reading.c` checks `sizeof(Reading) == 56` and `sizeof(Snapshot) == 584` at compile time (528 before the SGP30 replaced the MQ-7). |
| `kSensorCount` / `kMetricCount` | `SENSOR_COUNT` / `METRIC_COUNT` |
| `cfg::taskSpec(id)` | `cfgTaskSpec(id)`, which returns `const TaskSpec*` |
| HAL classes (`I2cBus`, `OneWireBus`, `AdcChannel`, `PulseCapture`) | A struct plus `i2cBusBegin(&bus, ...)`-style functions. Initialise statics with `I2C_BUS_INIT`, `ONE_WIRE_BUS_INIT`, `ADC_CHANNEL_INIT` or `PULSE_CAPTURE_INIT`. Members ending in `_` are private. |
| `SensorDriver` abstract class | A struct holding a `const SensorDriverOps*` table of function pointers. Each driver struct embeds `SensorDriver base` as its **first** member. Callers use `sensorDriverInit(drv)`, `sensorDriverStart(drv, now)`, `sensorDriverPoll(drv, now)`, ... on `&driver.base`. |
| Driver constructor `Bme280Driver d(config)` | `bme280DriverConstruct(&d, &config)`. Call it before any other use. In bring-up mains it is the first line of `setup()`. |
| `TdsDriver::setWaterTemperature(r)` | `tdsDriverSetWaterTemperature(&tds, &r)` |
| References `const Reading&` | Pointers `const Reading*` |
| `Wire` (I2C) | `esp32-hal-i2c.h` on controller 0, with the same 5 ms per-transaction timeout (`I2C_BUS_TIMEOUT_MS`) |
| `Serial.printf` / `Serial.begin(115200)` | `printf` to the UART0 console. It is already at 115200 (`CONFIG_ESP_CONSOLE_UART_BAUDRATE`), so no begin call is needed. |
| `void setup()` / `void loop()` | Defined in C. `hal/arduino_entry.h` binds them to the C++ symbol names the Arduino core calls (`_Z5setupv`, `_Z4loopv`). Include it in every file that defines `setup()`/`loop()`. |

To write a new driver, copy the pattern in `drivers/bme280.c`. Write `static`
functions taking a `SensorDriver*`, put them in one `static const SensorDriverOps`
table, and set `self->base.ops` to that table in `<name>DriverConstruct()`.

`NimBLE-Arduino` and `ArduinoJson` stay pinned in `platformio.ini`. NimBLE-Arduino is
used only by the C++ shim `src/transports/ble_gatt.cpp` (production images, WP8);
ArduinoJson is not used by project code (JSON is built with `snprintf` in
`domain/protocol_codec`).

## Layout (§7)

```text
Firmware/
  platformio.ini            pinned build + test_<sensor> envs + native
  include/board_profile.h   pins, rails, dividers; unresolved = unassigned  [FROZEN]
  include/app_config.h      periods, budgets, queue sizes, task table        [FROZEN]
  src/main.c                bootstrap only; loop() blocks forever
  src/domain/               pure C: Reading model, alarms, calibration       [types.h, reading.h FROZEN]
  src/drivers/              sensor_driver.h contract + one pair per sensor   [sensor_driver.h FROZEN]
  src/hal/                  clock, I2C, 1-Wire, ADC, pulse capture, safe GPIO, arduino_entry.h
  src/services/             acquisition, safety, power, rail epoch, snapshot, config, supervisor
  src/transports/           wifi_mqtt, ble_transport, protocol (only place JSON is built)
  src/diagnostics/          logger, progress counters
  test/bringup/             bringup_harness.h + one <sensor>_main.c per sensor
  test/native/              host unit tests for src/domain
  docs/architecture/        ReportLab source of Firmware Architecture.pdf
```

## Layer rules (do not break these)

- **Drivers** talk to hardware and report status. No MQTT, BLE or Wi-Fi includes. No task creation. No power-mode decisions. No relay writes. No hidden long delays: `start()` begins work and `poll()` returns `Pending` plus `nextPollMs`.
- **`src/domain/`** must not include `Arduino.h` or FreeRTOS headers. It has to compile on a PC (`env:native`).
- **Single owners:** acquisition owns the sensor buses, Safety owns the relay outputs, WiFiMqtt owns the MQTT client, and Configuration owns NVS.
- **Readers of sensor data** peek at the latest `Snapshot` with zero wait. They never wait for a measurement.
- **Health values:** a missing or invalid value is never reported as `0`. Use `Health` (`HEALTH_WARMING`, `HEALTH_TIMEOUT`, `HEALTH_UNPOWERED`, `HEALTH_UNCALIBRATED`, ...).
- **Timing:** use `monoNowMs()` for every age and deadline. Never use wall-clock time.

## Frozen contracts

These five files are shared by everyone:

- `board_profile.h`
- `app_config.h`
- `domain/types.h` (`Snapshot.readings[]` is indexed by `MetricId`)
- `domain/reading.h`
- `drivers/sensor_driver.h`

The C migration rewrote all five in C syntax: identifiers changed, but values,
fields and field order did not. The team lead should review that once.

To change one, open a short review with the team lead. Do not edit them inside a feature branch. Each change needs a hardware or measurement reason.

**Frozen-header change (28 Oct 2026, project owner's decision; team lead to be
told): `domain/types.h` gains `UNIT_LUX`**, appended after `UNIT_PPB`, so every
existing unit value is unchanged and `Reading` / `Snapshot` keep their sizes
(56 / 584 B, checked at compile time). Reason: the light sensor can now be
calibrated on the device against a lux meter (Calibration page) and then
publishes lux.

**Contract note (25 Oct 2026, audit D6):** every driver sets
`Reading.lastGoodMs` for each *usable* result, `HEALTH_OK` or
`HEALTH_UNCALIBRATED`; the `reading.h` comment says so. Comment-only change:
no field, order or size change.

## Work packages

| WP | Scope | Files | Stage | Blocked by |
|----|-------|-------|-------|------------|
| WP1 | **BME280** + I2C HAL | `drivers/bme280.*`, `hal/i2c_bus.*`, `test/bringup/bme280_main.c` | 2 | — (**running on S3 hardware 8 Oct 2026: ID 0x60 @0x76, 250 ms cadence, took 11 ms; fault/recovery tests pending**) |
| WP2 | DS18B20 + 1-Wire HAL | `drivers/ds18b20.*`, `hal/onewire_bus.*`, `domain/ds18b20_codec.*` | 2 | — (**driver + bit-banged 1-Wire HAL written 24 Oct 2026, 12-bit, Match ROM, CRC and 85 degC checks; running on S3 hardware 24 Oct 2026: ROM 28-00000098BB42, 762 ms per reading, 0 faults in ~200 readings; reference-thermometer check, unplug and supply-loss tests pending** — see "DS18B20 bring-up") |
| WP3 | DFR0026 light + ADC HAL | `drivers/light.*`, `hal/adc_channel.*` | 2 | — (**driver written 23 Oct 2026, publishes module mV (uncalibrated, no lux); running on S3 hardware 23 Oct 2026: dark 0 mV, room ~65 mV, clips at 3157 mV; unplug behaviour and LED-flicker spread not yet measured** — see "DFR0026 light bring-up") |
| WP4 | HC-SR04 + pulse capture (GPIO edge ISR + `esp_timer`) | `drivers/hcsr04.*`, `hal/pulse_capture.*`, `domain/ultrasonic.*` | 3 | ECHO divider verified (**driver written 24 Oct 2026, nominal 343 m/s, no temperature compensation; not yet run on hardware** — see "HC-SR04 bring-up") |
| WP5 | pH + TDS, calibration | `drivers/ph.*`, `drivers/tds.*`, `domain/calibration.*`, `domain/tds_codec.*` | 3 | — (**pH driver + ADC HAL written 17 Oct 2026, builds; not yet run on hardware, uncalibrated** — see "pH-4502C calibration". **TDS driver written 24 Oct 2026, nominal datasheet curve (uncalibrated); running on S3 hardware 24 Oct 2026: air 0 mV, tap ~1245 mV, hot ~1922 mV, salted water over range (FAULT); compensation checked with the DS18B20 in `test_water` (switched to ppm/EC compensation, ~5 % across 11-34 degC); reference solution not yet tested** — see "SEN0244 TDS bring-up") |
| WP6 | SGP30 air quality (TVOC, CO2eq) on the shared I2C bus | `drivers/sgp30.*`, `domain/sgp30_codec.*`, `hal/i2c_bus.*` (raw write/read), `test/bringup/sgp30_main.c`, `test/bringup/climate_main.c` | 2 | — (**running on S3 hardware 22 Oct 2026: feature set 0x0020 @0x58, 1 s cadence, 13 ms per measurement, no CRC faults; breath and hand-sanitiser spikes seen. Runs with the BME280 on the shared bus in `test_climate`, with humidity compensation. Baseline save/restore, 12 h learning and fault recovery not yet tested**; baseline is RAM-only until WP8 stores it in NVS) |
| WP7 | Acquisition scheduler, Safety, alarm rules, snapshot | `services/acquisition.*`, `services/safety.*`, `domain/alarm.*`, `domain/schedule.*`, `domain/safety_core.*`, `include/policy_config.h` | 4 | — (**domain logic done 25 Oct 2026**: rule evaluation with hysteresis/persistence/staleness, absolute-deadline job table with skip-not-catch-up, pure Safety state machine with lamp policy, stale → OFF and DEENERGIZE interlock; 41 host tests. **Services written 25 Oct 2026, builds, not yet run on hardware:** AcqNormal runs all 7 drivers as the single worker (D15); Safety task owns the relay (`hal/relay.c`); `main.c` boots safe outputs → logger → Safety → acquisition → Supervisor. Readings are stamped with the rail epoch; since WP9 the 5 V sensors are also gated on its validity (with transitions on). Thresholds still placeholders: rules ship **disabled** in `policy_config.h`; lamp auto-control **enabled**) |
| WP8 | Wi-Fi/MQTT, BLE, protocol, configuration, desktop app update | `transports/*`, `services/configuration.*`, `services/flash_gate.*`, `domain/protocol_codec.*`, `domain/frame_codec.*`, `domain/config_blob.*`, `domain/event_journal.*`, `domain/backoff.*`, `domain/sensor_set.*` | 5 | — (**8.0** contract v2 and desktop app v2 done; **Spike A** NimBLE pairing run on hardware 25–27 Oct 2026. **8.1** domain logic done, host-tested. **8.2** Configuration + NVS done and bench-tested 27 Oct 2026 (see "Configuration and NVS"). **8.3** BLE transport in every production image, D14 under BLE passed (see "BLE transport"). **8.4** Wi-Fi station + read-only LAN link written 27 Oct 2026 and bench-tested. **28 Oct 2026, written, not flashed (no circuit):** the LAN link (TCP 47800, mDNS, token ops) is **removed**: the app connects over Bluetooth only and Wi-Fi is a controller setting. **On-device calibration** for pH, TDS and light (`get_calibration`, `cal_capture`, `set_calibration`, `reset_calibration`; ConfigBlob schema 2 with schema-1 migration; see "On-device calibration"). No server/MQTT yet) |
| WP9 | Power ISR, rail epoch, lease handover | `domain/power_mode.*`, `services/power_coordinator.*`, `services/rail_state.*`, `services/acquisition.*` | 6 | — (power sense on GPIO8 measured 25 Oct 2026. **Written 28 Oct 2026, host-tested (8 state-machine tests), builds, NOT run on hardware yet:** see "Low-power mode (WP9)". Kill switch `POLICY_POWER_TRANSITIONS_ENABLED`) |
| WP10 | Logger queue, supervisor, watchdogs | `diagnostics/*`, `services/supervisor.*` | 4 | — (**written and run on S3 hardware 25 Oct 2026; stall tests PASS** (see "Production run"): logger queue + SerialLogger task (drop and count); cross-core progress stamps; Supervisor checks progress deadlines (Safety 1 s, AcqNormal 2 s, logger 2 s when busy), loss/lateness counters, stack high-water marks and internal heap; AcqNormal and Safety subscribed to the 5 s task watchdog, each resetting it only for itself; `stall_acq` / `stall_safety` envs for the §19 stall test) |

WP2, WP3, WP6 and WP10 can start in parallel. A driver counts as done when it meets the "Completion evidence" column of the §16 table. Compiling is not enough.

## Commissioning gates (hardware, §20)

The pins below are now on the drawing, but their behaviour must be measured on
the board before the matching feature is switched on. Do not guess the open items in code.

- External-power sense on GPIO8: **measured 25 Oct 2026** (`test_power_sense`): levels, decay and bounce pass; flag set. Open: HIGH ceiling by calculation only, loaded rail, slow pull. The divider has no hysteresis, so WP9 qualifies edges for 20 ms.
- Per-branch 5 V rail feedback: not on the drawing; use measured settle timing.
- Relay on GPIO15 (active-high, IRLZ44N): which contact (NO/NC) is used, the driven load and its safe state. Breadboard answer (`test_lamp`): grow lamp on NO, de-energised = lamp OFF; PCB still pending.
- SGP30 module: 3.3 V supply and logic levels, pull-ups, baseline storage policy.
- Light ADC headroom (the DFR0026 can exceed the S3's ~3.1 V range), and the pH range measured on the board (nominally 3.0 V max at the pin)
- All trip and clear thresholds

## Build and test

Run these commands from `Firmware/` with PlatformIO installed (VS Code extension
or `python -m pip install platformio` in a Python environment).

```bash
pio run -e esp32dev
```

```bash
pio run -e test_bme280 -t upload -t monitor
```

```bash
pio test -e native
```

Each `test_<sensor>` env links all production code but replaces `src/main.c`
with `test/bringup/<sensor>_main.c`. The harness runs the driver from a
core-1, priority-4 task (the same placement as AcquisitionNormal). It prints
every reading with its health and the measured start-to-ready time. While a
driver is still a stub, it prints `init -> NOT_IMPLEMENTED`.

The native env needs a host gcc on `PATH`.

The BLE bench scripts in `tools/` use the desktop app's Python environment.
After [setting up the app](../BluetoothControlApp/Readme.md), run them from
`Firmware/` with `../BluetoothControlApp` as the app-directory argument. See
the usage at the top of each script for the remaining arguments.

If the first build fails:

- **`build_src_filter` rejects `../test/bringup/...`:** move the bring-up mains to `src/bringup/`, point the filters there, and add `-<bringup/>` to the `esp32dev` and `native` filters.
- **Toolchain or path errors:** the project path contains spaces. Try setting `PLATFORMIO_CORE_DIR` to a path without spaces. This is something to check, not a confirmed problem.

## Production run (Stage 4, `esp32dev`)

```bash
pio run -e esp32dev -t upload -t monitor
```

> **This image switches the relay by itself.** About 30 s after boot in a dark
> room, Safety energises GPIO15 (lamp ON). Test with a low-voltage load on
> COM/NO first. The 230 V hookup follows "Lamp relay bring-up": supply
> unplugged while wiring, module enclosed, supervisor check before power-on.
> Lamp auto-control can be switched off with `POLICY_LAMP_AUTO_ENABLED 0` in
> `include/policy_config.h` (Safety then reports the decision but keeps the
> relay off).

Boot prints the board gate, `safety: started`, `acq: started` and one
`acq: <sensor> init OK` per driver, then the reset reason. Every 5 s Supervisor
prints two lines: the readings (`~` = UNCALIBRATED, a health name = no value)
and `lamp ON/OFF (reason) | latched | frames / late / skip / jitter | guard / fail |
health OK` (or `DEGRADED <first cause>` if anything went wrong in those 5 s).
A failed reading with a raw value shows it, e.g. `dist TIMEOUT(raw 30012)` =
ECHO rose but no echo came back; plain `dist TIMEOUT` = ECHO never rose.
Relay changes and lamp faults are logged as they happen. Supervisor also logs
`sup: <task> no progress for N ms` (once per stall), `sup: counters up: ...`
when a late/skip/guard/overflow/drop counter rises, and every 60 s the stack
free bytes per task and the internal heap (`sup: stack free B ...`).

Bench checks (not done yet):

| Check | Expected |
|-------|----------|
| Status line | every metric shows a value or an explained health |
| Cover the light sensor | `relay ON (level)` after the 30 s dwell |
| Torch on the sensor | `relay OFF (level)` after the 30 s dwell |
| Hold inside the 40-120 mV band | no relay change |
| Reset / power-cycle | relay OFF through boot |
| Steady state, 30 min | `guard 0 fail 0`, `late 0`, jitter <= 5 ms |

Stall test (§19): `pio run -e stall_acq -t upload -t monitor` (or `stall_safety`).
After 60 s the task blocks forever. Expected for `stall_acq`: `acq: INJECTED
STALL`, lamp `stale` -> OFF within ~3 s, `sup: acq no progress for 2xxx ms`,
task-watchdog panic at ~5 s, then `reset reason: TASK_WDT` after the reboot.
For `stall_safety`: `sup: safety no progress for 1xxx ms`, the relay keeps its
last level until the reset at ~5 s, then OFF through boot. Reflash `esp32dev`
afterwards.

Alarm latency test (§19 "Temperature alarm"): `pio run -e alarm_latency -t upload
-t monitor`. A test rule trips on every valid BME280 sample (4 per second, so
every frame phase) and de-energises the relay. Every 20 TRIPs Safety logs
`latency #N (20 TRIPs): submit->action us min/avg/max | sample->action ms
min/avg/max`. Target: sample -> Safety action <= 10 ms. Reflash `esp32dev`
afterwards (the test rule keeps the relay off).

Hardware results, 25 Oct 2026 (breadboard, sensors in air):

| Check | Measured |
|-------|----------|
| Boot | all 7 drivers `init OK`, `health OK`; 276 frames with `late 0 skip 0 jitter 0 ms guard 0 fail 0` |
| Lamp | dark -> ON; torch -> OFF at once (ON dwell over); dark again -> ON **30.0 s** after the OFF (OFF dwell); 2 relay changes in 3 min, no chatter |
| `stall_acq` | stall 60.34 s; `sup: acq no progress for 2227 ms` 62.51 s; `safety: lamp stale` 62.51 s; TWDT names `AcqNormal (CPU 1)` at 65.44 s; reboot `reset reason: TASK_WDT` |
| `stall_safety` | stall 60.35 s; `sup: safety no progress for 1202 ms` 61.33 s; TWDT names `Safety (CPU 0)` at 65.53 s; reboot `reset reason: TASK_WDT` |
| Stack free (min, B) | Safety 1348 / 3072 (after its first log call), AcqNormal 2304 / 4096, SerialLogger 896 / 2048, Supervisor 1016 / 3072 (600 before its line buffers were made static) |
| Heap (internal) | free 342 KB, minimum 337 KB, largest block 303 KB |
| `alarm_latency` (§19) | **PASS**: 120 TRIPs over 30 s, every BME280 phase. Acquisition submit -> Safety action 37-96 us (avg ~54 us); sample end -> action 1 ms (1 ms clock resolution, so < 2 ms) vs the 10 ms target. `late 0 skip 0 guard 0`, `health OK` throughout. Radios not running yet: repeat under Wi-Fi/BLE load in WP8 (D14) |

Not yet run: hold inside the 40-120 mV band and reset with the lamp ON (need the MOSFET
stage: the relay module does not release from a 3.3 V GPIO); 24 h endurance
(not planned for now). Safety's float log path (rule
TRIP/CLEAR lines) has not executed yet, so its stack figure excludes it.

Known limits: if the light S wire is pulled, the 1 uF capacitor decays to ~0 mV,
which reads as darkness and switches the lamp ON; firmware cannot detect it.
Alarm rules are all disabled (no commissioned thresholds). Since WP9, 5 V
readings are gated on the rail epoch, but only while
`POLICY_POWER_TRANSITIONS_ENABLED` is 1 (off until the WP9 bench check).

## BLE Spike A (WP8 Stage 8.0, historical)

> The `test_ble_spike` env and `test/bringup/ble_spike_main.c` were removed in Stage 8.3: their task became `src/transports/ble_transport.c` and runs in every production image (see "BLE transport"). The commands and results below are kept as the spike's record.

```bash
pio run -e test_ble_spike -t upload -t monitor
```

This is the production image (Safety, acquisition and Supervisor, **the relay switches as in Stage 4**)
plus one extra task, `BleSpike`, on the BleTransport slot (core 0, priority 2). The task serves the
contract-v2 GATT map through `src/transports/ble_gatt.cpp`. That file is the one C++ file in the
project, because NimBLE-Arduino is a C++ library; it exports a C API (`ble_gatt.h`) and is compiled only
with `-D FW_ENABLE_BLE`. Production `esp32dev` does not link NimBLE yet.

> History: this spike and its test steps used passkey pairing. Since firmware 0.11.0 the link is open
> (see "BLE transport" below); the passkey, bonds and BOOT-10 s steps no longer apply.

What the spike does:

- **Advertising and pairing.** Advertises `Hydro-ESP1` with the service UUID. Pairing uses LE Secure
  Connections, bonding and MITM, with the ESP as display-only. Boot prints
  `ble: PAIRING PASSKEY nnnnnn`, a new value on every boot.
- **Access.** Every characteristic needs an authenticated link (`*_AUTHEN`). The first bonded computer is
  the owner; a second computer that pairs gets its bond deleted and is disconnected.
- **Traffic.** State goes out every 2 s and is also the readable value. Telemetry (all 10 metrics from
  the live snapshot) goes out every 3 s. Both are sent as notifications in MTU-sized chunks.
- **Commands.** `get_sensors` is answered. Other valid operations get `unknown_op`, because
  Configuration/NVS is Stage 8.2. Malformed frames get the parser's error.
- **Bond recovery.** Holding BOOT (GPIO0) for 10 s **while running** clears all bonds. Holding it
  through reset enters download mode instead.
- **Diagnostics.** Every 10 s it logs `ble:` lines: secure link, bonds, connects and disconnects,
  pairing accepted and rejected, frames sent and dropped, longest frame send, notify failures,
  internal heap (free, min, largest block) and stack headroom of `BleSpike` and `nimble_host`.

At boot it also logs the NimBLE host task's core and priority. NimBLE-Arduino pins the host to core 0;
compare its priority with Safety's (6), which is the D14 question.

**Test with the desktop app on Windows:**

1. Flash, open the monitor and note the passkey.
2. Run `python Main.py` (no `--demo`). Open Connection, choose Scan, and check that `Hydro-ESP1` is listed.
3. Type the passkey and choose Connect. The monitor should show
   `ble: owner paired and secure (chunk N bytes)`.
4. The Dashboard fills with live readings. Metrics starts recording.
5. The app reports "Connected, but the controller did not answer: unknown_op", because
   `get_lan_token` is not in the spike. This is expected.
6. Disconnect, then connect again **without** a passkey: the Windows bond is reused.
7. Wrong passkey: pairing fails, the app asks again, and `pair rej` increases.
8. Second computer: pairing is rejected. Hold BOOT 10 s, then pair again.

Record from the `ble:` lines while the app is connected:
- heap (free, min, largest), compared with the radios-off baseline of 342 / 337 / 303 KB;
- `maxSend`;
- `notifyFail` and `dropped`.

Also record the Supervisor `jitter` and `late` counts under BLE load.

**First hardware run (25 Oct 2026):** paired from the Windows app with the in-app passkey. Measured over about 8 min connected:

| Check | Result |
| --- | --- |
| Pairing | `owner paired and secure`, chunk 244 B (MTU 247), bonds 1, accepted 1, rejected 0 |
| Commands | `get_sensors` -> ok, `get_lan_token` -> `unknown_op` (expected) |
| Traffic | ~8.3 frames / 10 s (State 2 s + Telemetry 3 s), dropped 0, notify failures 0, longest frame send 2 ms |
| Acquisition under BLE load | frames on time, late 0, skip 0, jitter 0 ms, health OK |
| Internal heap | free 238.4 KB, min 236.6 KB, largest block 221.2 KB, against 342 / 337 / 303 KB with radios off: NimBLE costs ~104 KB heap |
| Stack free | BleSpike 3732 / 6144 B, nimble_host 1380 / 4096 B (watch), Safety 1316 B after the first relay switch |
| NimBLE host task | core 0, **priority 21**. Safety (6) was also on core 0 at the time; it has been on core 1 since 28 Oct 2026 (see the D14 Wi-Fi results) |

The same run showed two problems:
- **Advertising overflow.** The log printed `NimBLEAdvertisementData: Cannot add UUID, data length exceeded!`: the name filled the main advertising packet and the service UUID fell back to the scan response. Fixed: the name now goes in the scan response, the UUID in the main packet.
- **Repeated boots when the monitor opened (not reproduced since, see below).** The first boot after flashing printed several partial boots, each `reset reason: POWERON`. They stopped at different points, some before the BLE start, so the cause is open. It could be the monitor's DTR/RTS toggling on this USB-UART, or the supply sagging (sensors, relay and radio all on USB). To tell them apart: open the monitor on `esp32dev` (no BLE) and see whether the repeats also happen there; if they appear only with BLE, measure 3.3 V during radio start.

**Follow-up checks (27 Oct 2026, fixed spike, driven by the app's own `BleLink`/`WinPairing` code):**

| Check | Result |
| --- | --- |
| Advertising | no more `Cannot add UUID` errors in any boot |
| Reconnect without passkey | connected in 2.2 s on the Windows bond; `get_sensors` ok; 6 State + 3 Telemetry frames in 10 s |
| Wrong passkey (after removing the Windows bond) | refused in 1.9 s (`Pairing failed`); ESP `rej` +1, no data served |
| Right passkey afterwards | paired in 3.6 s, ESP `pair ok` 2, bonds 1; data flowing |
| Repeated boots | not reproduced in 6 boots (production and spike, controlled reset and monitor-style port opening) |

Two observations for Stage 8.3:
- **MTU timing.** At `owner paired` the MTU exchange may not have happened yet (`chunk 20 bytes`); it reaches 244 shortly after. So the first frames go out in 20-byte chunks. Harmless, but slower.
- **Re-pairing deletes the old bond first.** When the bonded computer pairs again, NimBLE's default repeat-pairing handling deletes the old bond before the new pairing is authenticated. So a wrong passkey from the owner's address leaves no bond (`bonds=0` until the right passkey is used). Someone spoofing the owner's address could therefore force a re-pair, but could not take over without the passkey. Decide in 8.3 whether to override `BLE_GAP_EVENT_REPEAT_PAIRING`.

Not tested yet: second computer (owner lock) and BOOT 10 s bond clear (needs a hand on the button).

Build record (25 Oct 2026):

| Image | RAM (static) | Flash |
| --- | --- | --- |
| `esp32dev` | 45 488 B | 304 845 B |
| `test_ble_spike` | 72 996 B | 557 173 B |

So NimBLE adds about 27.5 KB of static RAM and 252 KB of flash.

## Wi-Fi station and LAN link (WP8 Stage 8.4)

> **28 Oct 2026: the LAN link below was removed.** The app connects over
> Bluetooth only; the station, its status in State, `set_wifi` / `forget_wifi`
> and the WP9 stop/start hooks stay. `wifi_lan.cpp` no longer uses ESPmDNS;
> a stop request now puts the radio in `WIFI_OFF` and `wifiMqttRadioOff()` is
> the acknowledgement PowerCoordinator waits for. A left-over `lan_tok` NVS key
> is erased once at boot. The text below is the 8.4 record.

The task is "WifiMqtt" (core 0, priority 2, 6 KB stack) in `src/transports/wifi_mqtt.c`, over the C shim `wifi_lan.cpp` (Arduino `WiFi` + `ESPmDNS`). It is in every production image (`FW_ENABLE_WIFI`, set together with `FW_ENABLE_BLE` in `[production]`). It never waits for an access point, and a Wi-Fi failure never touches sensing or Safety. There is no MQTT and no server yet; the task name is kept from the architecture's task table.

**Station.**
- Credentials come from Configuration (`set_wifi` from the app's Wi-Fi page, over Bluetooth). A change or `forget_wifi` restarts the station at once.
- Each join attempt has 15 s. A failed attempt reports `failed` and retries with exponential backoff: 1, 2, 4 … 60 s, ±20 % jitter (`domain/backoff`). A lost connection retries from 1 s.
- `WiFi.persistent(false)` keeps the credentials out of the Wi-Fi driver's NVS storage; Configuration is their only store.
- Modem sleep stays on, because Bluetooth shares the radio.
- `wifiMqttRequestStop/Start` are the WP9 power-transition hooks.

**mDNS.** While connected, the board announces `_hydro._tcp` on port 47800 with TXT `node=Hydro-ESP1` and `v=2`; the host name is `hydro-esp1`.

**LAN link (read-only).**
- TCP port 47800, at most 2 clients.
- A client must send `hello` with the 32-hex LAN token within 5 s. The token is compared in constant time; if none has been issued yet, nobody gets in. A wrong or missing token gets `unauthorized` and the socket is closed.
- After the hello, the client receives the same frames as Bluetooth (`transports/frames.c`, same boot id, `"link":"lan"`): State every 2 s, Telemetry every 3 s, alarm events.
- Any command gets `ble_only`.
- Telemetry is skipped when the socket is not writable. A client that cannot take a whole frame is dropped, so the stream is never corrupted.
- Rotating the token (over Bluetooth), losing Wi-Fi or a stop request closes every client.

**State.** `wifi.state` is `unconfigured`, `connecting`, `connected` or `failed`. When connected, the frame also carries the IP and RSSI. The app stores the IP and uses it if mDNS is blocked.

**Supervision.** A 2 s progress deadline, stack check and task watchdog. Every 60 s a `wifi:` line logs state, IP, RSSI, joins, failures, backoff, LAN clients, refusals, drops and skipped telemetry.

**Known limits:**
- The LAN link is plaintext. It carries read-only data, and the token only keeps casual clients out (contract v2).
- The PHY calibration data stored by the Wi-Fi driver in NVS (namespace `phy`, written by the IDF at first start) is a second framework writer besides the NimBLE bond store. Like the bond store, those writes do not take the flash gate.

**Bench tool.** `tools/lan_bench.py` was removed with the LAN link (28 Oct 2026); the results below are its 27–28 Oct run. `tools/ble_bench.py stream N` keeps the Bluetooth link busy for radio-load tests.

**Hardware run (27–28 Oct 2026):**

| Check | Result |
| --- | --- |
| Failure path (`tools/wifi_fail_bench.py`, non-existent network) | each attempt 15 s, then `failed`; retries after 0.96, 2.04, 4.10, 6.58 s (1/2/4/8 s ±20 %); `forget_wifi` -> `unconfigured` at once |
| Real join (credentials from the app's Wi-Fi page) | joined in 12.5 s (172.16.1.27, RSSI −58 dBm); 2.6–3.6 s from boot on later boots |
| App with link preference Auto | switched to the LAN link by itself 2 s after the join |
| `lan_bench.py checks` | wrong token -> `unauthorized` + closed; right token -> data; command over LAN -> `ble_only`; same boot id on both links; rotate -> LAN client dropped, old token refused, new one accepted |
| BLE + LAN streaming 100 s | BLE 55 State + 36 Telemetry, LAN 50 + 33, no drops |

**D14 Safety under Wi-Fi + BLE load (`alarm_latency`).** Per 20 trips:

| Phase | submit -> action | sample -> action |
| --- | --- | --- |
| **Boot, Wi-Fi radio starting** | **38.8–109.8 ms (one trip per boot)** | **40–110 ms ✗** |
| Wi-Fi connected, idle | max 502 us | max 1 ms |
| + BLE streaming | max 113 us | 0 ms |
| + BLE and LAN streaming | max 544 us | max 1 ms |

**FAIL at Wi-Fi start-up.** Three instrumented boots (a `latency SPIKE` line now prints any trip over 5 ms in the test build) show one slow trip of 40–110 ms per boot, always within ~50 ms of `wifi: joining`, and none afterwards. The Wi-Fi driver's start-up (RF calibration at high priority on core 0) starves Safety on core 0.

**The D14 fallback fixes it.** With Safety on core 1 (first tried with a test-only override), the same three boots stay at max 145 us submit -> action and ≤ 1 ms sample -> action, with acquisition still 0 late frames and ≤ 1 ms jitter.

**Decision (28 Oct 2026, project owner): Safety moved to core 1 in the frozen task table** (`include/app_config.h`: `{"Safety", 1, 6, 3 * 1024}`). The team lead should be told, because it changes a frozen contract. Core 0 now holds the radios (Wi-Fi driver, NimBLE host, TCP/IP), BleTransport, WifiMqtt, Configuration and Supervisor. Core 1 holds Safety (priority 6), AcqNormal (4) and SerialLogger (1). Re-test results are below.

**Re-test with Safety on core 1 from the task table (28 Oct 2026, `alarm_latency`, 3 boots):**
- **Safety:** `safety: started (core 1, prio 6)`, and no `latency SPIKE` in any boot.
- **First report window (radio start at 0.77 s):** worst submit -> action 97 / 167 / 96 us; sample -> action ≤ 1 ms.
- **Acquisition:** 0 late frames, jitter ≤ 1 ms.
- **Production `esp32dev` (90 s):** health OK; stack free saf 2232, acq 2376, cfg 2212, ble 3660, wifi 4300 B; internal heap with both radios on 153.5 KB free (min 153.2 KB, largest block 139 KB).
- **Wi-Fi did not join in these runs**, retrying with backoff: 15 s timeouts, then about 1, 2, 4.6 and 7.7 s waits. The radio still started at 0.77 s, so the start-up case D14 is about was covered. The steady state with Wi-Fi connected was measured before the move (max 544 us); repeat it on core 1 once the network is reachable again.

**Connected-state run on core 1 (28 Oct 2026, network `mohsen`, `alarm_latency`, 180 s):**
- **Setup:** Wi-Fi connected 2.9 s after boot (RSSI −32 dBm). The desktop app reconnected over Bluetooth by itself at 10.8 s and streamed until the end.
- **Result:** 35 reports of 20 trips each. Worst submit -> action 152 us, sample -> action ≤ 1 ms, no `latency SPIKE`. Acquisition: 0 late frames, jitter ≤ 1 ms.
- **D14 passes on core 1 with Wi-Fi and BLE both up.**
- **LAN port check:** reachable from the PC on the same network; a wrong token gets `unauthorized`.
- **Not covered:** in this run the app did not open its LAN link, so LAN streaming load was not part of it. LAN streaming was part of the earlier core-0 run (max 544 us).

## BLE transport (WP8 Stage 8.3)

Every production image (`esp32dev`, `stall_*`, `alarm_latency`) defines `FW_ENABLE_BLE` and starts the BLE transport after acquisition. Bring-up envs do not define it and never link NimBLE. If the BLE transport fails to start, sensing and Safety keep running; only the app link is lost.

The task is "BleTransport" (core 0, priority 2, 6 KB stack), in `src/transports/ble_transport.c` over the C shim `ble_gatt.cpp`.

**Link (open since 0.11.0, 12 Nov 2026, written, not flashed).**
- No pairing, no bonding, no passkey: `setSecurityAuth(false, false, false)`, IO capability none, no `*_AUTHEN` permissions, no Auth gate characteristic. The passkey, the single bonded owner and the BOOT 10 s bond clear are removed.
- Why: a bond tied the ESP to the first computer. Switching computers needed BOOT 10 s and a reset, the passkey had to be copied from the serial log, and a computer whose bond the ESP had dropped kept failing until the device was removed in Windows Settings.
- Bonds stored by older firmware are deleted at boot, so NimBLE no longer writes NVS.
- One client at a time: a second client is disconnected at once (`refused`), and advertising restarts only once the slot is free.
- After connecting, the ESP requests a 15-30 ms interval and a 4 s supervision timeout, so a vanished client frees the slot quickly.
- A channel is notified only after the client subscribed to it (so a frame never starts before the app listens). "Ready" = subscribed to State, Telemetry and Result. A client not ready 20 s after connecting is dropped (`idle`).
- Trade-off: the link is not encrypted; telemetry, commands and `set_wifi` credentials are readable in radio range.

**Pending hardware checks for 0.11.0 (not flashed yet):**
1. Boot logs `ble: advertising as Hydro-ESP1 (open link, no pairing)`. No passkey line appears.
2. App on PC X: Scan, then Connect with no passkey. The log shows `client connected and subscribed`, and the first State and Telemetry frames decode with no error.
3. X disconnects. PC Y sees the ESP at once and connects.
4. X's laptop vanishes (lid closed, or Bluetooth turned off). Y sees and connects within about 5 s. The supervision request was accepted if this takes about 4 s, not about 20 s.
5. A Windows PC still paired with 0.10 connects from the app with no step in Windows Settings (the `--log` file shows `removed the old Windows pairing`). Check whether Windows briefly auto-connects in the background first.
6. A second client is refused: `refused` increases by 1.
7. nRF Connect connected without subscribing is dropped at 20 s: `idle` increases by 1.
8. `ble_tear` still resyncs cleanly. App reconnect after the ESP is power-cycled.

**Traffic and priority.**
- Order of service: commands and their results, then alarm events (from the Safety journal, from link-up on), then State (every 2 s and at link-up; also the readable value), then Telemetry (every 3 s).
- A telemetry frame that cannot be queued is dropped and superseded by the next one (`tlmDrop`).
- Results retry for up to 200 ms per chunk (`resDrop` if that fails).

**Commands.**
- `get_sensors` is answered here.
- `get_calibration` and `cal_capture` are answered here too (zero-wait peeks of the applied config and the snapshot).
- `get_diagnostics` is answered here from the Supervisor's hardware snapshot (see "Hardware diagnostics").
- `set_sensor_enabled`, `set_wifi`, `forget_wifi`, `set_calibration` and `reset_calibration` go to Configuration; `reset_latch` goes to Safety. (Up to 8.4 also `get_lan_token` / `rotate_lan_token`, removed 28 Oct 2026 with the LAN link.)
- Up to 8 commands are in flight; more get `busy`.
- Each request carries the link's session generation and a 5 s expiry, so a reply is never sent on a newer link and a command is never applied late. Reply tags carry a sequence number, so a late reply cannot reach a reused slot.

**Supervision.** A 2 s progress deadline, stack check and task watchdog (the task resets it only for itself). Every 60 s a `ble:` line logs `ready`, connects, disconnects, `refused`, `idle`, frames, `tlmDrop`, `resDrop`, `busy`, `maxSend` and `notifyFail`.

**Known limits (decided in 8.3):**
- **No link security (0.11.0).** See "Link" above. Whoever connects first owns the session until they disconnect.
- **NimBLE bond store.** Since 0.11.0 it is only cleared once at boot, if older firmware left bonds. Nothing is bonded afterwards.
- **NimBLE host priority.** The host runs on core 0 at priority 21. Safety has been on core 1 since 28 Oct 2026 (D14 with Wi-Fi), so the radios no longer share its core.
- **Wi-Fi state.** Since 8.4, State reports the real station state (see "Wi-Fi station and LAN link").

**Hardware run (27 Oct 2026, `esp32dev`, bench `tools/ble_bench.py` over the app's own `BleLink`):**

| Check | Result |
| --- | --- |
| Boot | `ble: started (core 0, prio 2), NimBLE host core 0 prio 21`; passkey printed once; no WARN/ERROR |
| Config across a reflash | the revision stored by the 8.2 bench (rev 3) loaded unchanged |
| Bench `before` | TDS off -> `cfg_rev 4`, null/STALE; token stable, rotate changes it; `set_wifi` stored; bad rule -> `bad_field`; State reports `fw 0.8.3`, `sgp30_baseline learning` |
| Reboot, bench `after` | rev 4, TDS off, SSID and token all kept; restore -> rev 5, TDS back |
| Health (60 s report) | stack free ble 3508 / 6144 B, cfg 2028 / 4096 B; heap 228.6 KB (min 226.8 KB); frames late 0 / skip 0 |
| Congestion | burst of replies: `notifyFail 8`, `maxSend 36 ms`, `tlmDrop 1` (superseded telemetry), `resDrop 0` |

**D14 Safety under BLE load (`alarm_latency`, 27 Oct 2026).** A test rule trips on every BME280 sample (4/s). Results per 20 trips:

| Phase | Reports | submit -> action (us) | sample -> action (ms) |
| --- | --- | --- | --- |
| BLE advertising, no client (0-48 s) | 9 | avg 72-77, max 123 | max 1 |
| Owner connected, State + Telemetry streaming + a command every 2 s (48-145 s) | 20 | avg 71-84, max 137 | max 1 |

**PASS:** worst case 1 ms against the 10 ms target, so the D14 fallback (Safety on core 1) is not needed for BLE. Repeat this with Wi-Fi up in 8.4: the TCP/IP task (priority 18) is also on core 0.

**Bench tools** (in `tools/`, use PlatformIO's Python for `serial_log.py` and the app's venv for `ble_bench.py`):
- `serial_log.py PORT FILE SECONDS [--reset]`: logs the port without the monitor's DTR/RTS toggling. `--reset` gives exactly one controlled boot.
- `ble_bench.py APP_DIR before|after|stream N`: runs the configuration bench before and after a reboot (sensor switch, Wi-Fi credentials, latch reset, persistence), or keeps a busy link for N seconds. The LAN token steps were dropped with the LAN link.

## Configuration and NVS (WP8 Stage 8.2)

`services/configuration.c` is the only NVS writer. Its task is "Configuration": core 0, priority 2, 4 KB stack. It is fed by an 8-entry request queue; a full queue answers `busy`, never false success.

NVS namespace `hydro`:

| Key | Content | Written |
| --- | --- | --- |
| `cfg_a` / `cfg_b` | `ConfigBlob` schema 2 (magic, schema, revision, sensor mask, `calOverridden`, lamp policy, pH / TDS / light calibration, CRC-32) | on a change, into the slot not holding the current config; boot loads the newest valid slot. A schema-1 slot (up to 8.4) is migrated: revision and sensor mask kept, calibrations from the defaults |
| `wifi` | SSID and password | `set_wifi` / `forget_wifi`; never logged or sent back |
| `lan_tok` | (removed 28 Oct 2026) | erased once at boot if an 8.4 image left it |
| `sgp_base` | SGP30 baseline | offered by acquisition hourly once valid; written only if it changed, at most once an hour |
| `rst_hist` | last 8 `esp_reset_reason()` codes | once per boot, 30 s after start (never during a reset loop) |

**Set by command:** the sensor mask (`set_sensor_enabled`) and, since 28 Oct 2026, the pH / TDS / light calibrations (`set_calibration`, `reset_calibration`; see "On-device calibration"). The lamp policy still comes from `include/policy_config.h`: `configBlobRefreshDefaults()` copies it, and every calibration whose `calOverridden` bit is clear, from the source on every boot. A re-measured value in the source therefore wins over an old copy in flash (the revision goes up when it changes), but never over a calibration made on the device.

**Boot.** `configurationInit()` runs before Safety and acquisition. It loads the config, publishes it, and returns. If NVS fails, the compiled defaults apply: all sensors on, nothing stored.

**Applying a change.** A change is applied by AcqNormal itself at the next frame boundary (no other task touches a driver):
- a disabled sensor is cancelled and no longer scheduled; its readings age to STALE and the protocol sends `null`;
- a re-enabled sensor is initialised again.

Configuration replies `ok` only after acquisition acknowledges the new revision (≤ 3 s), otherwise `timeout`. The revision is stamped into every Reading (`configRevision`).

**Safety rules for sensor changes.** Switching a sensor off is refused (`rule_latched`) while one of its alarm rules is latched or active (`safetyRulesBlockSensor`, §12). A disabled sensor's *unlatched* rules raise no stale alarm. A latched rule stays visible until its sensor is back and a valid recovery sample arrives.

**Flash gate.** `services/flash_gate.c` keeps NVS writes out of the HC-SR04 echo window: a flash write stalls the cache on both cores, which would delay the echo interrupt.
- Acquisition only *tries* the gate (zero wait) and defers the ping.
- Configuration waits up to 100 ms per try.
- Bring-up builds never create the gate, so it counts as free there.

**Safety commands.** `safetySubmitCommand()` (4-entry queue) carries `reset_latch` with an expiry, so a latch reset is never applied late. Safety also keeps the 64-event alarm journal (`safetyJournalNext()`, one independent cursor per link; only BLE since the LAN link was removed).

**Supervisor.** It now also watches Configuration: a 3 s progress deadline, its stack, and the 5 s task watchdog, which Configuration resets itself, including while it waits.

**Bench.** At the time of the 8.2 bench, `test_ble_spike` routed the app's commands to Configuration and Safety, so the Sensors and Wi-Fi pages work end to end. There is still no Wi-Fi station (8.4): stored credentials show as `connecting`.

**Bench run (27 Oct 2026, `test_ble_spike`, driven over BLE by the app's own `BleLink`):** (record of that
firmware: the LAN token rows no longer apply since 28 Oct 2026, and the ConfigBlob is now schema 2)

| Check | Result |
| --- | --- |
| First boot on erased config | `cfg: rev 1, sensors 0x7f` (defaults stored) |
| `set_sensor_enabled tds false` | `ok`, `cfg_rev 2`; `acq: sen0244_tds disabled` 1.0 s later (next frame); TDS telemetry `null` / STALE; State `enabled` without `tds`; telemetry `cfg_rev 2` |
| Same request again | `ok`, revision unchanged (no flash write) |
| `get_lan_token` twice, then `rotate_lan_token` | same token twice; rotate gives a new one |
| `set_wifi` | `stored`; State shows the SSID, `connecting` (no station until 8.4) |
| `reset_latch` with rule 5 | `bad_field` |
| **Reboot** | `cfg: rev 2, sensors 0x5f, wifi set, LAN token set`; TDS still off from boot; same token; same SSID |
| Re-enable TDS, `forget_wifi` | `cfg_rev 3`, TDS re-initialised and back to values; Wi-Fi `unconfigured` |
| Reset history | written once, 30 s after boot: `1 0 …`, then `1 1 …` (1 = POWERON) |
| Health during the run | no WARN/ERROR lines, frames late 0 / skip 0 / jitter 0 ms, stack free cfg 2208 / 4096 B, heap free 228.7 KB (min 227.0 KB) |
| Distance during flash writes | ~3.47 m throughout; the one 566 mm value came at t = 10 s, before any write. The 5 s summary is too coarse to prove "no glitch", so a ping-by-ping log during writes is still to do |

Notes:
- **Notify retries.** A burst of six command replies caused 6 notify retries (`notifyFail=6`, `maxSend 27 ms`) with **0 frames dropped**. The 8.3 transport should keep the retry loop.
- **Not covered on hardware.** The refusal to disable a sensor while its rule is latched: all rules ship disabled, so it is covered only by the host tests (`test_safety_disabled_sensor_cannot_silence_alarm`).

## pH-4502C calibration (WP5)

Wiring: PO -> 10 kOhm -> GPIO7 (ADC1_CH6), 15 kOhm and 1 uF to GND. DO and TO
are not connected. A 5 V module output is 3.0 V at the pin, inside the S3's
~3.1 V range at 11 dB (the ~2.45 V limit in Rev 1-2 was for the original ESP32).

The driver averages a spaced window: 10 conversions 30 ms apart, sorted, mean of
the middle 6 (the tutorial's filter). Until two buffer points are entered it
publishes the module voltage in mV with `HEALTH_UNCALIBRATED` (D13). The
bring-up harness prints those values with `(uncal)`.

```bash
pio run -e test_ph -t upload -t monitor
```

Each reading is followed by a `module NNNN mV` line: the module PO voltage
used for the fit, printed whatever the pH line shows (the nominal fit is the
default, see below).

1. **Offset (once per module).** Unplug the probe and short the BNC centre to
   its shell. Turn the pot nearest the BNC until the module line reads about
   `2500 mV`, which is pH 7. A factory-set module puts pH 7 near 0 V,
   so it first prints `ph=UNPOWERED` (PO below about 170 mV). Check that the
   5 V rail is on, then keep turning the pot until mV values appear.
2. **Buffers.** Plug the probe in. Rinse it, put it in the pH 6.86 buffer, wait
   at least 2 minutes and note the module mV. Rinse it again, then do the same with
   pH 4.01. Never move the probe between solutions without rinsing.
3. **Fit.** Enter both mV values as `PH_BUFFER_A_MV` and `PH_BUFFER_B_MV` in
   `test/bringup/ph_main.c` and flash it again. The slope is printed at boot.
   The tutorial's nominal slope is about -0.0057 pH/mV. Readings then show
   `pH` with health OK.
4. **Check.** Measure a third buffer (for example 9.18) that was not used for
   the fit, and record the error and the noise. Recalibrate from time to time,
   because probes drift.

**Without buffers (nominal pH).** While both buffer values are 0, `ph_main.c`
uses a nominal fit and prints pH with `(uncal)`. pH 7 is the module voltage
measured with the BNC shorted (`PH_NOMINAL_MV_AT_PH7`, 2758 mV on 17 Oct 2026),
and the slope is the tutorial's -0.0057 pH/mV. Expect roughly +/-0.5-1 pH, and
measure the shorted value again whenever the offset pot is turned. The driver
reports these readings as `HEALTH_UNCALIBRATED` (`calibrationProvisional`), so
they never count as actionable. Bring-up tests on 17 Oct gave tap water 6.9,
vinegar 3.6 and baking soda 7.8.

Production keeps the compiled nominal fit until the user calibrates on the
device (next section); after that the stored fit is used and pH reads `OK`.

## On-device calibration (WP8, 28 Oct 2026; written, not flashed)

The app's **Calibration** page calibrates, on demand, the three sensors whose
reading depends on the individual probe. The fit is stored in the ConfigBlob
(NVS), survives reboots and firmware updates, and is applied by acquisition at
the next frame boundary (`applyConfiguration()` -> `phDriverSetCalibration` /
`tdsDriverSetCalibration` / `lightDriverSetCalibration`).

| Sensor | Points | x (raw, from `cal_capture`) | Fit, plausibility (`policy_config.h`) | After |
| --- | --- | --- | --- | --- |
| pH | 2 buffers >= 1 pH apart | module mV (pin mV through the 10k/15k divider) | two-point line, slope negative, \|slope\| 0.002-0.02 pH/mV (swapped buffers refused) | pH `OK` (no longer provisional) |
| TDS | 1 reference solution | ppm at 25 degC on the nominal curve, before K | K = ref / x, 0.5-2.0 | TDS `OK` |
| Light | 1 lux-meter reading | pin mV | lux per mV through zero, 0.01-100 | light in `lux` (`UNIT_LUX`), `OK` |

- **Ownership.** `cal_capture` never touches a driver or the ADC: BleTransport
  peeks the snapshot and converts the `raw` (filtered pin mV) the drivers
  already publish (`domain/cal_capture`, host-tested). It refuses with `stale`
  when the sensor is off, unpowered, faulty, older than 5 s, or (TDS) without
  a fresh water temperature.
- **Defaults.** `configBlobRefreshDefaults()` still copies the compiled values
  at boot, except for a calibration whose `CAL_BIT_*` is set in
  `calOverridden`. `reset_calibration` restores the compiled default and clears
  the bit.
- **Safety.** Refused (`rule_latched` / `rule_active`) while an alarm rule on
  that sensor is latched / active, like switching it off.
- **Lamp.** The lamp thresholds stay in light-sensor pin mV: `lamp_control`
  uses `Reading.raw` once the value is lux, so calibrating never moves the lamp.
- **Not calibrated here:** BME280 and DS18B20 (factory-calibrated), SGP30 (own
  baseline in `sgp_base`), HC-SR04 (distance; a tank-height mapping and a
  DS18B20 offset are possible follow-ups).
- **Not hardware-tested yet.** On the bench: calibrate pH in 7.00 / 4.00
  buffers, reboot, check `cfg: ... calibrated 0x01` and pH `OK`.

## Low-power mode (WP9, 28 Oct 2026; written, not flashed)

Before this date `power_coordinator.c` was a stub: State always reported
`"mode":"unknown"`. Only the GPIO8 sense itself had been measured
(`test_power_sense`).

- **Decisions:** `domain/power_mode.c`, a pure state machine with 8 host tests
  (`test/native/test_power`): boot paths, 50 ms loss debounce
  (`CFG_POWER_LOSS_DEBOUNCE_MS`; the 20 ms `BOARD_POWER_SENSE_QUALIFY_MS` was
  the bench qualification only), 500 ms stable return, quiesce -> RELEASED ->
  grant order, DEGRADED on a missing acknowledgement (200 ms worker, 2 s Wi-Fi)
  without ever granting a second owner, and abort of a return on a new loss.
- **Task "PowerCoord"** (core 0, priority 5, 4 KiB) feeds it every 10 ms while
  anything is pending (100 ms otherwise). The GPIO8 edge ISR runs on core 1,
  where AcqNormal installed the GPIO ISR service: it invalidates the rail
  (new epoch), disables itself (a loss chatters 479-928 edges) and notifies
  the task, which re-arms it after 50 ms of quiet level.
- **Loss:** AcqNormal quiesces (pending steps cancelled; 5 V drivers
  `preparePowerOff`, HC-SR04 TRIG held low) and releases; AcqLowPower (core 0)
  gets the lease with BME280, DS18B20, light and SGP30; Wi-Fi goes to
  `WIFI_OFF`; Safety drives the relay OFF (`lamp_reason` "no 5 V": the coil
  has no supply). pH, TDS and HC-SR04 publish `UNPOWERED`. BLE stays up.
- **Return:** 500 ms of stable power, then the low-power worker releases, the
  rail is restored, AcqNormal gets every sensor back (they re-init and show
  `WARMING`), Wi-Fi rejoins.
- **Rail gating:** a 5 V sensor starts only while the rail is valid, and its
  result is kept only if the rail is still valid with the same epoch at the end.
  State's `power_epoch` is now the live rail epoch.
- **Deviation from §15:** Safety stays on core 1 (D14 move, 28 Oct 2026), so
  core 1 is not idle in low power. CPU frequency and light sleep are unchanged.
- **Kill switch:** `POLICY_POWER_TRANSITIONS_ENABLED` in `policy_config.h`.
  **Shipped as 0 since 28 Oct 2026 (audit)** until the bench check below:
  no ISR, every sensor on the normal worker, the mode only reports the level.
  Set it to 1 for the bench check.
- **Polling fallback:** if the GPIO ISR cannot be installed (transitions on),
  PowerCoord samples the pin every 10 ms and invalidates the rail itself on a
  level change (up to one tick later than the ISR would).
- **Bench checks to run once the circuit exists:** unplug the 5 V feed with the
  app connected: NORMAL -> TRANSITION_TO_LOW -> LOW_POWER within ~0.3 s, Wi-Fi
  off, pH / TDS / level `UNPOWERED`, relay off, BLE still up; replug: back to
  NORMAL after ~0.5 s, sensors WARMING then OK, Wi-Fi rejoins. Also: count ISR
  wake-ups per unplug (should be one per storm), and repeat D14 with Wi-Fi
  connected.
  - Every transition switches the Wi-Fi driver off (`WIFI_OFF`) and back on:
    the same radio start that starved core 0 in D14. Measure the core-0 stall
    during a transition and watch the heap over many unplug cycles.
  - In low power the DS18B20 1-Wire timing (interrupts masked) runs on core 0
    next to the BT controller: check that BLE stays stable, since it is now
    the only link.
- **Before setting transitions to 1:** GPIO8 has no pull. Without the sense
  divider wired, the board boots into low power (or flaps if the pin floats).
  Wire and check the divider first. This also applies to `alarm_latency` and
  the stall test envs. A spurious edge on GPIO8 turns the lamp off for at
  least `POLICY_LAMP_MIN_OFF_MS`: watch for that on the real wiring.

## Hardware diagnostics (30 Oct 2026; written, not flashed)

The app's Dashboard shows a "Controller hardware" section through the
`get_diagnostics` command, introduced in firmware 0.10.0. See the
[BLE contract](../BluetoothControlApp/FirmwareContract.md#hardware-diagnostics).

- **Nothing new is measured on request.** Supervisor already computed heap and
  stack high-water marks every 10 s for its log lines. It now also keeps them
  in a snapshot, adds PSRAM, NVS usage (every 60 s, read from NVS's RAM page
  table, never a flash write) and the app partition size (once at boot).
  BleTransport copies the snapshot into a static view and encodes it into its
  existing `s_data` buffer (worst case checked < 1536 B by a native test).
- **CPU load per core** (`diagnostics/cpu_load.c`): FreeRTOS run-time stats are
  off in the Arduino prebuilt sdkconfig, so each core's idle hook executes the
  WAITI itself, counts the halted cycles and returns `false` so IDF does not
  halt again. The cores sleep exactly as before; nothing busy-waits. Supervisor
  turns the counts into load % every second and keeps a 60 s peak.
- **Not included:** chip temperature. On IDF 4.4 the ESP32-S3 temperature
  sensor shares the SAR hardware with the ADC that reads pH, TDS and light, and
  `temperatureRead()` reconfigures it on every call. Add it only after a bench
  check that those readings are not disturbed.
- **No frozen header changed** (`app_config.h` is only read for the task names,
  cores and stack sizes).
- **Bench checks to run on the board:** compare the Dashboard heap and stack
  figures with the serial `sup: stack B ... | heap ...` lines. At rest, the CPU
  load should be in single digits. To check the load figure itself, run a
  temporary low-priority task pinned to core 1 that busy-waits 500 ms out of
  every 1000 ms: core 1 should read about 50 % above its rest value. (The
  `stall_*` envs block their task, so they do not load the CPU.)

## DS18B20 bring-up (WP2)

Wiring: DQ -> GPIO14 with 4.7 kOhm to 3.3 V, sensor on the retained 3.3 V supply
(external power, not parasite mode). The 1-Wire bus is bit-banged on an
open-drain pin (`hal/onewire_bus.c`), with standard-speed timing from the
datasheet. Each time slot (<= 70 us) runs with interrupts off on that core.
The 480 us reset-low phase runs with interrupts on.

`init()` finds the first device with Search ROM, checks the ROM CRC and the
family code (28h), and from then on addresses only that ROM (Match ROM). If the
EEPROM holds a lower resolution, it writes 12-bit to the scratchpad only, never
to EEPROM, and does this again on every init. Each 1 s frame: `start()` sends
Convert T, the driver waits the full 750 ms, then reads the 9-byte scratchpad
once (~12 ms).

| Situation | Reading |
|-----------|---------|
| Valid scratchpad, -55..+125 degC | value in degC, `HEALTH_OK`, raw = register (1/16 degC) |
| Scratchpad CRC wrong, or config fixed bits wrong (all-zero read) | `HEALTH_FAULT`, no raw |
| Register = 0550h (+85 degC power-on value) | `HEALTH_FAULT`; the next frame re-inits |
| Outside -55..+125 degC | `HEALTH_FAULT` |
| No presence pulse | `start`/`poll -> NOT_PRESENT`, result becomes `HEALTH_FAULT` (lastGoodMs kept), re-init next frame. `init -> NOT_PRESENT` leaves the result unchanged |
| `init -> FAULT` repeatedly on a sensor that answers | the scratchpad config byte fails the fixed-bit check; suspect a clone part |

The TDS driver takes this reading through `tdsDriverSetWaterTemperature()`.
`test_water` (see "Water probes together") passes it across in bring-up, and
production acquisition does the same before every TDS start (`prepareInputs()`).

```bash
pio run -e test_ds18b20 -t upload -t monitor
```

WP2 is done once these are filled in:

| Check | Measured |
|-------|----------|
| ROM code printed at boot | `28-00000098BB42 crc C7` (24 Oct 2026, `test_ds18b20`) |
| Room air vs reference thermometer | 26.00-26.12 degC in room air; no reference thermometer yet |
| Ice water (0 degC) / warm water | tap water ~24.8 degC (still falling slowly); hot water settled at 47.19 degC. Air -> hot water: 24.8 -> 43 degC in ~12 s. Hot -> tap water: 43 -> 27 degC in ~18 s. Ice water not tried yet |
| Line-to-line spread | +/-0.06 degC (one 12-bit step) when settled |
| DQ unplugged, then replugged (recovery) | not measured yet |
| Supply removed, then restored (85 degC check) | not measured yet |
| Start-to-ready time printed by the harness (~760 ms expected) | 762 ms on every reading; no FAULT/NOT_PRESENT in a 3 min run with water changes |

## Water probes together (`test_water`)

`test/bringup/water_main.c` runs the DS18B20 and the TDS driver from one task
with a 1 s frame, like `test_climate` does for the I2C pair. Each frame:

1. It passes the latest DS18B20 reading, from the previous frame, to the TDS
   driver with `tdsDriverSetWaterTemperature()`. A faulty reading is passed
   as is, so the TDS driver reports `NO TEMP` instead of using it. Since 25 Oct
   2026 (audit L2) a `NOPRES` frame also writes a FAULT result, so TDS shows
   `NO TEMP` from the next frame on instead of using the old value for 3 s.
2. It starts both drivers and polls them in turn. The 750 ms DS18B20
   conversion and the ~560 ms TDS window overlap, so both fit in one frame. Run
   one after the other, they would take ~1.3 s.

TDS is therefore compensated with a water temperature about 1 s old, inside
the 3 s staleness limit. The first frame has no temperature yet, so it shows
`NO TEMP`.

```bash
pio run -e test_water -t upload -t monitor
```

Output is one table row per second, with the header repeated every 20 rows:

```text
        | DS18B20 |  SEN0244 TDS                                |
   time | water T |    raw | uncomp |   @25C | state   | trend    |
      s |    degC |     mV |    ppm |    ppm |         |          |
--------+---------+--------+--------+--------+---------+----------+
     12 |   24.81 |   1245 |    470 |    466 | UNCAL   | stable   |
     55 |   51.94 |   1922 |    825 |    465 | UNCAL   | stable   |
     90 |  NOPRES |   1921 |    824 |      - | NO TEMP | stable   |
```

| Column | Meaning |
|--------|---------|
| water T | DS18B20 in degC, or `FAULT`, `NOPRES` (no presence pulse), `TIMEOUT`, `-` (no result yet) |
| raw | TDS pin mV (filtered window) |
| uncomp | ppm from raw without temperature compensation |
| @25C | ppm compensated to 25 degC, the value production uses; `-` without a fresh water temperature |
| state | `OK`, `UNCAL` (nominal curve, no K-factor), `NO TEMP`, `OVER` (above 1000 ppm or ADC clipped), `FAULT` |
| trend | `stable` once the shown ppm (`@25C`, else `uncomp`) stops drifting: over the last 10 rows, the newer 5 average within 0.4 % (min 2 ppm) of the older 5, and no row is more than 2 % off. Else `settling` |

In a new cup, `raw` and `uncomp` jump within a second, but the DS18B20 takes
~20 s to get most of the way to the water temperature, then creeps for a minute
or more. Until then, `@25C` is off (it can even show a brief false `OVER`). Read
the row once `trend` says `stable`. Same tap water, cold and hot: `uncomp` differs, `@25C` should
match. Events (init, faults, ROM code, compensation on/off, first timings)
print as `# ...` lines between rows. Checks to record:

| Check | Measured |
|-------|----------|
| Compensation switches ON after the first frame | yes: frame 1 `NO TEMP`, frame 2 compensated (24 Oct 2026) |
| Same solution at two temperatures (compensated ppm should match within ~10 %) | tap water at 11.4 degC (1234 mV), 33.6 degC (1768 mV), 11.6 degC (1278 mV): 631 / 620 / 653 ppm after the change to ppm compensation (~5 %). Recomputed from the logged raw mV and temperatures; the first firmware compensated the voltage and gave 683 / 584 / 713 |
| Over range | salted water 2380-2386 mV -> `OVER` at 22-24 degC |
| DS18B20 unplugged: TDS falls back to `NO TEMP` (within ~3 s) | not measured yet |
| No `TIMEOUT (frame guard)` lines over a long run | none in ~4.5 min |
| Re-run with the ppm-compensated firmware and the new trend rule | 24 Oct 2026, read at `stable`: tap water 16.0 degC / 1467 mV -> 688 ppm; warm 42.0 degC / 1957 mV -> 634 ppm (8 % apart; `uncomp` 565 vs 849). Salt: raw pinned at 2378-2386 mV from 42 down to 25 degC, so the module output is clipped (~2.38 V) and the ppm is meaningless; `OVER` throughout once compensated, correct. `stable` took ~30 s (cold), ~55 s (warm), ~45 s (salt) |

## SEN0244 TDS bring-up (WP5)

Wiring: A -> GPIO6 (ADC1_CH5), 100 nF to GND, module on the filtered switched
5 V rail. There is no divider, so pin mV is the module output (0-2.3 V nominal).
The probe is waterproof but the board and connectors are not, and the probe must
stay in water below 55 degC, away from the container wall (datasheet).

The driver averages 15 conversions 40 ms apart (sorted, mean of the middle 5,
~560 ms per window). The datasheet sample takes the median of 30 taps 40 ms apart,
which is ~1.2 s and would not fit the 1 s frame. The conversion is in
`domain/tds_codec.c` and is host-tested:
`ppm = (133.42 V^3 - 255.86 V^2 + 857.39 V) x 0.5 / (1 + 0.02 (T - 25))`.

The temperature compensation divides the **ppm** (conductivity), as DFRobot's
GravityTDS library does. The datasheet's sample sketch divides the **voltage**
before the curve instead. The curve is not linear, so that over-corrects. With
the same tap water at 11.4 / 33.6 / 11.6 degC (`test_water`, 24 Oct 2026):

| Method | @25C ppm (cold / warm / cold) | Spread |
|--------|-------------------------------|--------|
| Voltage compensated (datasheet sketch, first version) | 683 / 584 / 713 | ~20 % |
| ppm compensated (GravityTDS, current) | 631 / 620 / 653 | ~5 % |

`TdsConfig.calibration` is an optional K-factor on that ppm (slope = K, offset 0),
to be fitted against a reference solution. raw is always the filtered pin mV.

| Situation | Reading |
|-----------|---------|
| ADC clipped, or ppm above the rated 1000 ppm | value kept, `kQualitySaturated` + `HEALTH_FAULT` |
| No OK DS18B20 reading younger than 3 s, or outside 0-55 degC | uncompensated value, `kQualityUncompensated` + `HEALTH_DEPENDENCY_INVALID` |
| Compensated, no K-factor | ppm, `HEALTH_UNCALIBRATED` (nominal curve, D13) |
| Compensated, K-factor set | ppm, `HEALTH_OK` |

The 0-1000 ppm range is below many hydroponic nutrient solutions (often
1000-1500 ppm or more). Those readings are FAULT and never actionable. 0 V is
distilled water, a probe in air, an open lead or a dead rail alike, so the
driver cannot report `UNPOWERED`; that is left to acquisition (rail epoch, WP9).

`test_tds` runs the TDS driver alone, without the DS18B20 (use `test_water` for both), so it prints
`tds=DEPENDENCY_INVALID (raw <mV>)` by default. To see ppm, measure the water
with a thermometer and set `TDS_MANUAL_WATER_TEMP_C` in
`test/bringup/tds_main.c`. Bring-up then feeds that value as a fresh DS18B20
reading every frame and prints `<ppm> ppm (uncal)`. This option exists only in
bring-up.

```bash
pio run -e test_tds -t upload -t monitor
```

WP5 (TDS) is done once these are filled in:

| Check | Measured |
|-------|----------|
| Probe in air | 0 mV (24 Oct 2026, `test_tds`) |
| Distilled water | not measured yet |
| Tap water (with water temperature) | 1232-1261 mV (~465 ppm on the nominal curve if at 25 degC; water temperature not measured in this run). Back in the same cup after hot and salt: 1264-1272 mV (+2 %) |
| Reference solution (K-factor = reference / reading) | not measured yet |
| Nutrient solution / over-range sample | salted water 2378-2383 mV -> `tds=FAULT` (above 1000 ppm), as designed. Hot tap water: 1914-1935 mV, uncompensated ~825 ppm (water temperatures not measured in this run; see `test_water` for compensated values) |
| Line-to-line spread | about +/-10 mV settled (cold and hot); a few % drift in the first ~10 s after moving the probe |
| Start-to-ready time printed by the harness | 560 ms on every reading |

## DFR0026 light bring-up (WP3)

Wiring: S -> 10 kOhm -> GPIO1 (ADC1_CH0), 1 uF to GND (tau ~10 ms), module on the
retained 3.3 V supply. There is no divider, so pin mV is the module output.

The driver averages 10 conversions 11 ms apart (sorted, mean of the middle 6,
~100 ms per window). The spacing is not a multiple of 10 ms on purpose: lamps
flicker at 100/120 Hz and the RC only partly filters it, so each tap lands on a
different flicker phase. Every reading is module mV with `HEALTH_UNCALIBRATED`.
The datasheet gives no lux conversion, and `types.h` has no lux unit. Lux needs a
reference-meter mapping plus a reviewed `UNIT_LUX` (D13). A clipped tap (bright
light, ~3050 mV) sets `kQualitySaturated`, and the value is then a lower bound,
not a fault. The handover code's raw `analogRead` counts are not reused.

```bash
pio run -e test_light -t upload -t monitor
```

Record the mV for: covered (dark), room light, phone torch (clip point), the
line-to-line spread under LED/fluorescent light, and what happens when S is
unplugged (the 1 uF holds the last level; the driver cannot detect it). WP3 is
done once these are filled in:

| Condition | Measured (23 Oct 2026, `test_light`, 99 ms per window) |
|-----------|----------|
| Dark (covered) | 0 mV fully covered; ~21 mV (20.5-22) with a hand that lets some light in |
| Room light | ~65 mV in a dim room; ~324 mV (323-325, +/-1 mV) with brighter room lighting |
| Torch / saturation | rises through ~480-1190 mV, clips flat at 3157 mV (above the HAL's 3050 mV flag, so `kQualitySaturated` is set) |
| Spread under LED light | not measured yet |
| S unplugged | not measured yet |

Response time is within one 1 s frame in both directions (dark -> torch ~3 frames
while the torch was brought closer; torch -> covered 0 mV in ~3 frames).

## External-power sense bring-up (`test_power_sense`, WP9 gate)

Goal: measure what decides `BOARD_POWER_SENSE_VERIFIED`. The levels at GPIO8
with USB in and out, how long the pin decays through the undefined band, and how
many edges each plug or unplug gives. The divider has no hysteresis.
`power_sense_main.c` timestamps every GPIO8 edge from a CHANGE interrupt (us)
and samples ADC1_CH7 at 1 kHz. Once the level has been stable for
`BOARD_POWER_SENSE_QUALIFY_MS` (20 ms) it prints a `MODE: NORMAL` /
`MODE: LOW-POWER` banner, as the WP9 power task will decide. When the pin has
settled, three indented lines follow: result (PASS/FAIL), timing and bounce. A
heartbeat repeats the mode every 30 s. `v` shows each edge, `d` prints the
table of all transitions.

The ADC is set up once through the IDF driver. `analogReadMilliVolts()` must not
be used on a pin that also needs its digital input: every call runs
`pinMode(pin, ANALOG)`, which disables the input buffer. The test checks that
both paths still agree and prints a `WARNING` line when they do not (ISR deaf,
GPIO/ADC mismatch); then press `a` for digital-only mode. The ADC clips at ~3050 mV, so the high
level (~3.33 V) needs a multimeter at GPIO8: >= 2.475 V and <= 3.6 V.

Pass criteria, all needed before the flag goes true: multimeter HIGH in
2.475-3.6 V; steady LOW max <= 825 mV; every RISE/FALL settles at the right GPIO
level; the edge count and bounce span per transition are recorded (more than
one edge means the power ISR needs a qualification window); and the decay time is known.

```bash
pio run -e test_power_sense -t upload -t monitor
```

The env opens the monitor with DTR/RTS released, so reopening it after an
unplug does not reset the board and the transition history survives.

Hardware result, 25 Oct 2026 (breadboard; power bank on the + rail, ESP32 on
the PC USB). 6 power losses, 5 power-ons and 1 contact dip, over two runs:

| | Power lost (FALL) | Power on (RISE) |
|---|---|---|
| Settled GPIO level | 0 every time | 1 every time |
| Settled ADC | 0-53 mV (<= 825 mV) | 3157 mV, clipped (>= 2475 mV) |
| Edges / bounce span | 479-928 edges over 5.0-14.0 ms, gaps down to 1 us | 3-16 edges over 0.03-0.2 ms |
| Real switching point (ADC at the flip) | ~1650 mV | ~1930-1990 mV |
| Below VIH -> below VIL | 26-181 ms -> 300-453 ms after departure (band ~272 ms) | above VIH 2-4 ms, clip 14-15 ms |
| First GPIO edge after departure | 59-220 ms (+ rail already ~2.5 V) | < 1 ms |

- The divider without hysteresis chatters through ~1.65 V, giving up to ~930
  interrupts in 14 ms. The WP9 power ISR must disable itself on the first edge
  and let the power task re-read the level after a qualification window
  (>= 20 ms suggested, longer than the 14 ms worst case seen), then re-arm.
- The edge arrives when the 5 V branch is already down to ~2.5 V. 5 V readings
  started in the ~60-220 ms before the edge can be invalid, so the rail epoch
  must also reject readings that finished just before it.
- Unplugging can make contact bounce (an ADC dip with no GPIO edge). The digital
  input ignores it.
- A ~0.4 s cut of the 5 V (pin below 825 mV and back, most likely the power
  bank switching its output off and on by itself, not a manual replug) gave
  LOW-POWER then NORMAL 0.37 s apart, both correctly qualified. WP9 should
  expect such back-to-back transitions; returning to NORMAL also needs the 5 V
  branch settle time before the 5 V sensors are read.
- Power banks with an auto-off timer or low-load cut-off can drop the 5 V on
  their own, and the firmware then (correctly) goes LOW-POWER. A bank shutting
  down also decays more slowly at first (210 ms to below VIH, 780 ms to below
  VIL, against 26-181 / 300-453 ms for a pulled cable). If the field supply
  is a power bank, choose one with an always-on / low-current mode.
- The HIGH ceiling (<= 3.6 V) was not measured (no multimeter, ADC clipped):
  4.75-5.25 V x 440/660 = 3.17-3.50 V by calculation.
- The ADC and the GPIO input coexist on GPIO8 (`isr ok`, `mismatch 0`;
  1 `adc-disturbed` in the chatter of one transition).
- Not covered: the + rail under sensor load, a slow cable pull, the power bank's own button.
- 13 Nov 2026, ESP32 on the power bank alone (no PC USB): badly seated +/- rail jumpers made it
  brown out at boot (reset reason `BROWNOUT`, no BLE advertising, a blinking board LED). Wi-Fi join
  attempts made it worse. Fixed by reseating the rail cables. The app's Dashboard shows the last
  reset reason under **Uptime**: a red "Brownout" there means check the supply wiring first.

## Lamp relay bring-up (`test_lamp`, ahead of WP7)

Goal: lamp ON when ambient light is low, OFF when it is high. Breadboard only.
The PCB layout (IRLZ44N stage on GPIO15) is decided after this works, and
`board_profile.h` is unchanged.

Wiring: a TONGLING JQC-3FF-S-Z 5 V "Low Level Trigger" module. VCC to 5 V, GND
to ESP32 GND. IN through a transistor stage like the PCB: GPIO15 -> 220 Ohm ->
N-MOSFET gate (or 1 kOhm -> NPN base), 10 kOhm gate/base to GND, source/emitter
to GND, drain/collector to IN. GPIO15 HIGH = relay ON, so `lamp_main.c` drives
it active-high (`LAMP_RELAY_ACTIVE_LOW 0`), like `BOARD_RELAY1_ACTIVE_LOW`.
Light sensor as in `test_light`, on 3.3 V (on 5 V it can exceed GPIO1's range).
Mains: live (brown) to COM -> NO -> lamp, neutral (blue) straight to the lamp,
NC empty. Relay off (reset, no USB, battery) = lamp OFF.

Hardware result, 24 Oct 2026 (IN straight to GPIO15, no transistor):

- Module VCC not on a live 5 V read ~2.5 V: GPIO15 back-fed it through IN. Check
  VCC is about 5 V and the red PWR LED is on before connecting IN.
- With VCC fixed, covering the sensor (0-4 mV) switched the relay ON after the
  30 s dwell, but GPIO15 HIGH (3.3 V) never switched it OFF. The module's input
  is referenced to its 5 V VCC. Direct drive is ruled out for this module.
- Not yet run with the transistor stage: uncover -> OFF and reset -> OFF.

Test with a low-voltage load on COM/NO first. The 230 V hookup is done with
the supply unplugged, the module in an enclosure away from the ESP32, earth
on the fixture if it is metal (the feed cable is 2-core brown/blue), and
checked by a supervisor before power-on.

```bash
pio run -e test_lamp -t upload -t monitor
```

Decision: `domain/lamp_control` (host-tested). ON below `LAMP_ON_BELOW_MV`, OFF
above `LAMP_OFF_ABOVE_MV`, unchanged in between; each state is held at least
`LAMP_MIN_ON_MS` / `LAMP_MIN_OFF_MS` (boot counts as a switch to OFF). Saturated
counts as bright; `LAMP_FAULTS_TO_OFF` bad readings in a row switch OFF at once.
The 40 / 120 mV, 30 s values are bench placeholders.
Deliberate bring-up deviation from §10: it acts on uncalibrated mV and does not
check reading age, so a stalled sensor freezes the last state (possibly ON).
WP7 must calibrate light or accept relative mV explicitly, and add staleness -> OFF.

Checks: cover the sensor -> ON after the dwell; torch -> OFF after the dwell;
no chatter inside the band; reset or unplug -> OFF and stays OFF through boot.

Thresholds (not yet measured): the sensor may see the lamp it controls, which
would switch it off, then on, in a loop. At the final sensor position record
night lamp OFF, night lamp ON and daylight lamp OFF, and put
`LAMP_OFF_ABOVE_MV` above "night lamp ON". If that is above daylight, shield
the sensor from the lamp or point it at the window.

## HC-SR04 bring-up (WP4)

Wiring: module on the switched external 5 V. TRIG <- GPIO17 (3.3 V logic, inside
the module's TTL input range). ECHO -> 1 kOhm -> GPIO18, 2 kOhm to GND, so 5 V
at the module is ~3.33 V at the pin. **Check the divider before connecting
GPIO18:** with the module's ECHO wire disconnected, put 5 V on the module side
of the 1 kOhm and measure about 3.3 V at the junction (or scope the junction on
a running module). Never force the module's ECHO output to 5 V.

The ECHO pulse is timed by a CHANGE interrupt that stores `esp_timer`
microsecond timestamps (`hal/pulse_capture.c`). `pulseIn()` is not used. One
ping per 1 s frame, 10 us trigger, 30 ms timeout (`CFG_HCSR04_TIMEOUT_MS`).
Distance = width x 343 m/s / 2 (58.3 us/cm), with no air-temperature
compensation (~0.17 %/degC), so every reading carries `kQualityUncompensated`.
A missing distance is never 0:

| Situation | Reading |
|-----------|---------|
| Echo 20-4000 mm | value in mm, `HEALTH_OK` |
| Echo outside 2-400 cm | `HEALTH_FAULT`, raw = width in us |
| ECHO rose but did not fall within 30 ms (nothing in range) | `HEALTH_TIMEOUT`, raw = elapsed us |
| ECHO never rose (stuck low, unplugged, 5 V rail off) | `HEALTH_TIMEOUT`, no raw |
| ECHO high before the trigger (stuck high) | `init`/`start -> FAULT` |

There is no rail-feedback pin, so "rail off" cannot be told apart from
"stuck low". `UNPOWERED` is left to acquisition (rail epoch, WP9). Do not test
"rail off" with `test_hcsr04` running: the harness keeps pulsing TRIG high into
the unpowered module (§12).

```bash
pio run -e test_hcsr04 -t upload -t monitor
```

WP4 is done once these are filled in:

| Check | Measured |
|-------|----------|
| GPIO18 level with ECHO high (divider) | not measured yet |
| Flat target at 10 / 50 / 100 cm (tape vs reading, spread) | not measured yet |
| Open space (> 4 m) | not measured yet |
| ECHO unplugged at the module side (not at GPIO18: no pull, it floats) | not measured yet |
| ECHO held high | not measured yet |
| Target < 2 cm | not measured yet |
| Start-to-ready time printed by the harness | not measured yet |
| Mounting height above the tank, empty/full mapping | not measured yet |

## Stage 1 build record (4 October 2026)

PlatformIO Core 6.2.0 on Windows. The first build downloads about 1 GB of toolchain and takes ~8 min. Later builds take ~1 min.

| Env | Result | RAM | Flash |
|-----|--------|-----|-------|
| `esp32dev` | SUCCESS | 22 080 B (6.7 %) | 267 893 B (20.4 %) |
| `test_bme280` … `test_mq7` (7 envs) | all SUCCESS | ~21.5 KB (6.6 %) | ~269.5 KB (20.6 %) |
| `native` | 6/6 tests passed | — | — |

- There are no compiler warnings from `src/` or `include/`.
- The `../test/bringup/` source filter works as written.
- The project path contains spaces, but the build ran without problems.
- These numbers are for the stubs. Expect them to grow once the drivers, NimBLE and Wi-Fi are linked in.

**Resolved packages:**
- `framework-arduinoespressif32` 3.20017.241212 (Arduino-ESP32 2.0.17)
- `toolchain-xtensa-esp32` 8.4.0+2021r2-patch5
- NimBLE-Arduino 2.5.1
- ArduinoJson 6.21.5

**Resolved SDK configuration** (`tools/sdk/esp32/sdkconfig`, checked against §15):

| Setting | Value | Consequence |
|---------|-------|-------------|
| `CONFIG_FREERTOS_HZ` | 1000 | 1 ms tick. `pdMS_TO_TICKS` is exact. |
| `CONFIG_FREERTOS_UNICORE` | not set | Dual core. "Low-power single core" means only application affinity (§15). |
| `CONFIG_PM_ENABLE` | not set | No automatic DFS or light sleep with this framework build (§15). |
| `CONFIG_BTDM_CTRL_PINNED_TO_CORE` | 0 | BT controller on core 0, as §8 assumes. NimBLE host affinity is still to be checked in WP8. |
| `CONFIG_ARDUINO_RUNNING_CORE` | 1 | `loop()` task on core 1. It blocks forever. |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | 5 | Matches `CFG_TASK_WATCHDOG_MS`. |

Not yet done: flashing to a real board. Put the folder under git so the team can work on one branch per WP.

## C migration build record (16 October 2026)

Every env was built twice on the same machine and platform: once from the C++ tree
just before the port (baseline) and once from the C tree.

| Env | C++ RAM / Flash (B) | C RAM / Flash (B) | Result |
|-----|---------------------|-------------------|--------|
| `esp32dev` | 19 540 / 274 009 | 18 828 / 237 345 | SUCCESS |
| `test_bme280` | 19 680 / 304 665 | 19 044 / 273 333 | SUCCESS |
| `test_ds18b20` | 18 956 / 275 693 | 18 236 / 238 881 | SUCCESS |
| `test_light` | 18 956 / 275 693 | 18 236 / 238 897 | SUCCESS |
| `test_hcsr04` | 18 956 / 275 705 | 18 244 / 238 889 | SUCCESS |
| `test_ph` | 18 988 / 275 697 | 18 276 / 238 905 | SUCCESS |
| `test_tds` | 19 036 / 275 705 | 18 324 / 238 921 | SUCCESS |
| `test_mq7` | 18 980 / 275 705 | 18 268 / 238 909 | SUCCESS |
| `native` | 6/6 passed | 6/6 passed | PASSED |

- About 0.7 KB less RAM and about 31-37 KB less flash. The C++ `Wire` and
  `HardwareSerial` objects are no longer linked: I2C now goes straight to
  `esp32-hal-i2c`, and console output goes through `printf`. Data records have not
  changed size (checked at compile time in `reading.c`).
- There are no compiler warnings from `src/`, `include/` or `test/`. The
  platform-independent sources also compile cleanly on the host with
  `-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`.
- The C port of the BME280 compensation (`compensateT/P/H`) was compared on the host
  against the original C++ code. It gave identical results for 200 000 random
  calibration and ADC inputs.
- In the linked images, `setup`/`loop` resolve to the C definitions.
- **Console differences from `Serial`:** in this framework's S3 sdkconfig, `printf`
  ends lines with CRLF (`CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF`), where `Serial.printf`
  sent a bare `\n`. It also mirrors output to the native USB-Serial-JTAG port
  (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`). `pio device monitor` on UART0 at
  115200 is unaffected. Scripts that parse logs byte for byte should allow `\r\n`.
- **Re-verified on hardware since:** the C production image ran on the S3 on
  25 Oct 2026 with all 7 drivers `init OK` and `health OK` (BME280 included),
  over the UART console (see "Production run"). The `test_bme280` bring-up's
  fault/recovery checks are still pending.

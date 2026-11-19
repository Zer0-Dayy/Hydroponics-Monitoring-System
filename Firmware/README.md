# ESP32-S3 controller firmware

The integrated firmware identifies itself as **0.11.0** and targets the
**ESP32-S3-WROOM-1-N16R8** with 16 MB flash and 8 MB octal PSRAM. It acquires
seven sensors, controls the grow-lamp output, stores commissioning settings,
and serves the desktop app over [BLE contract v2](../BluetoothControlApp/FirmwareContract.md).

## Build and test

Install PlatformIO through the VS Code extension or with
`python -m pip install platformio` in a Python virtual environment. Put a host
GCC toolchain on `PATH` for the native tests. Run the following from the
repository root.

```sh
cd Firmware
pio run -e esp32dev
pio test -e native
```

`esp32dev` is the project name for the production ESP32-S3 image. The
[PlatformIO configuration](platformio.ini) selects `esp32-s3-devkitc-1` and
overrides the flash size, partition table, and QIO/OPI memory mode for N16R8.
It pins the Espressif 32 platform and the NimBLE-Arduino dependency. The first
build needs access to PlatformIO's package registry. The build produces
`.pio/build/esp32dev/firmware.bin` alongside the bootloader and partition image.
Use the PlatformIO upload target for the normal flash layout.

The native suites cover portable C logic, including sensor conversions,
calibration, alarm and lamp decisions, power transitions, framing, and
configuration storage records. Use them before flashing a change.

## Bench bring-up

1. Start with a compatible ESP32-S3 development board or a powered and
   electrically checked Rev 1.5 PCB. On the custom PCB, **J1 is USB Data** and
   **J3 is 5V In**. Keep the grow-lamp load disconnected for the first flash.
   Use the [PCB connector and power guide](../PCB%20Design/README.md#rev-15-connections).
2. With the USB data cable attached, run `pio device list` from `Firmware/`.
   Close other serial terminals that are using the port. For manual download
   mode on Rev 1.5, hold **BOOT**, tap **RST**, then release **BOOT**. This
   holds GPIO0 low through reset. [Espressif boot-mode guide](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html#manual-bootloader)
3. Connect the BME280 and upload its isolated bring-up image.

   ```sh
   pio run -e test_bme280 -t upload -t monitor
   ```

   The harness reports I²C probe results, chip ID, measurements, and health
   at 115200 baud. The harness tries both `0x76` and `0x77`. Production
   acquisition uses `0x76`. A BME280 reports chip ID `0x60`.
4. Check the other sensor branches with the matching environment below. Then
   upload the production image.

   ```sh
   pio run -e esp32dev -t upload -t monitor
   ```

   Look for Safety startup, sensor initialization, the reset reason, and
   `ble: advertising as Hydro-ESP1`. The production image can energize GPIO15
   automatically. For a sensing-only bench build, set
   `POLICY_LAMP_AUTO_ENABLED` to `0` in [policy_config.h](include/policy_config.h)
   before building.
5. Open the desktop app in live mode. On **Connection**, scan and connect to
   **Hydro-ESP1**. On **Dashboard**, check the power mode and sensor health.
   Use **Sensors** to review the enabled devices, then calibrate pH, TDS, and
   light against references. [Commissioning and calibration](../BluetoothControlApp/Readme.md#connect-to-a-controller)

### Bring-up and acceptance images

The bring-up images replace `src/main.c` with a focused test entry point.
They print measurements or timing on serial. Use the production image for
BLE commissioning.

| What to check | PlatformIO environment |
| --- | --- |
| Air and water sensors individually | `test_bme280`, `test_sgp30`, `test_ds18b20`, `test_hcsr04` |
| Analog inputs individually | `test_light`, `test_ph`, `test_tds` |
| Shared I²C, or water temperature and TDS together | `test_climate`, `test_water` |
| External 5 V sense and low-voltage relay load | `test_power_sense`, `test_lamp` |
| Watchdog recovery, alarm timing, and BLE frame recovery | `stall_acq`, `stall_safety`, `alarm_latency`, `ble_tear` |

The last row contains fault-injection builds for controlled bench
acceptance tests. Use `esp32dev` for deployment. The [bench notes](docs/bench-notes.md)
record the test setups and which hardware revisions were exercised.

### Diagnostics from the bench

- Run `pio device monitor -b 115200` to reopen the serial console. The
  Dashboard's **Controller hardware** section also shows uptime, restart
  reason, CPU load, RAM, and storage figures.
- A `BROWNOUT` reset points to supply voltage dropping. Check the rail and
  connections under load. The [power-sense records](docs/bench-notes.md#external-power-sense-bring-up-test_power_sense-wp9-gate)
  document an earlier bench case.
- The [BLE bench tools](tools/) use the app's Python dependencies. Run them
  from `Firmware/` with `../BluetoothControlApp` as the first argument.
  For example, use the app environment to stream a 90-second check on Linux.

  ```sh
  ../BluetoothControlApp/.venv/bin/python tools/ble_bench.py ../BluetoothControlApp stream 90
  ```

  Close the desktop connection first so the bench tool can claim the one BLE
  slot. The configuration and Wi-Fi failure benches write test settings to
  the controller. Use a dedicated bench controller for those checks.

## Power and safety policy

- The 3.3 V branch is designed to retain the ESP32-S3, BME280, SGP30, DS18B20,
  and DFR0026 during an outage. HC-SR04, pH, TDS, and the relay interface
  depend on external 5 V.
- The default build has `POLICY_POWER_TRANSITIONS_ENABLED` set to `0`.
  GPIO8 determines the reported power mode while the normal acquisition
  worker and Wi-Fi station remain enabled. Treat measurements from 5 V
  sensors as unavailable when their supply is lost. Full transitions
  invalidate those readings in firmware, hand acquisition to the low-power
  worker, and manage Wi-Fi shutdown and restart. Measure Rev 1.5 rail
  behavior and qualify GPIO8 before enabling full transitions. The existing
  `BOARD_POWER_SENSE_VERIFIED` flag refers to earlier breadboard measurements.
  [Power transition record](docs/bench-notes.md#low-power-mode-wp9-28-oct-2026-written-not-flashed)
- GPIO15 is active high. Safety checks the light reading, dwell times, rail
  validity, and interlock state before driving it. Lamp thresholds use
  ADC-pin millivolts even when calibrated light is displayed in lux.
  The default grow-lamp policy is on. The example alarm rules ship disabled
  and require commissioned trip and clear limits. [Policy definitions](include/policy_config.h)

## Firmware architecture

The firmware uses C with FreeRTOS and the Arduino-ESP32 hardware APIs.
Two C++ adapters expose NimBLE and the Arduino Wi-Fi station to the C
services. The normal acquisition worker and Safety run on core 1, away
from most radio work. The core assignments and timing budgets are in
[app_config.h](include/app_config.h).

| Area | Responsibility |
| --- | --- |
| [Bootstrap](src/main.c), [HAL](src/hal/), and [drivers](src/drivers/) | Put outputs in their safe state at boot. Handle bus, ADC, and GPIO access and convert measurements into typed readings. |
| [Acquisition and snapshot services](src/services/) | Own the sensor buses, schedule conversions, and publish the latest copied snapshot. Most sensors use a 1 s frame and the BME280 uses a 250 ms cadence. |
| [Safety and PowerCoord](src/services/) | Safety owns the relay and alarm latches. PowerCoord tracks external power and coordinates acquisition ownership. A rail generation counter identifies 5 V samples taken before a power edge. |
| [Configuration and domain logic](src/services/configuration.c) | Apply validated commands and store the sensor mask and probe calibration in versioned NVS records. The [portable domain layer](src/domain/) implements codecs, policies, and state machines for host testing. |
| [BLE and Wi-Fi transports](src/transports/) | BLE publishes State every 2 s and Telemetry every 3 s while subscribed. Results and alarm events share the Result characteristic. Wi-Fi joins the configured network as a station. |
| [Supervisor and diagnostics](src/services/supervisor.c) | Check task progress and watchdogs, collect stack and memory data, and publish the hardware snapshot read by the app. |

Every reading carries identity, units, health, and sample timing. Consumers
peek the latest snapshot without waiting for a conversion. Unusable values
are sent as JSON `null` with their health status. Keep relay writes in Safety,
sensor bus calls in the acquisition worker, and NVS writes in Configuration.
Keep the domain layer portable so the native suites can exercise it.

## Design references and bench records

- [Board profile](include/board_profile.h) defines firmware pins. Use the
  [Rev 1.5 schematic](../PCB%20Design/Hardware/PCB%20Design.kicad_sch) for
  component values and connector wiring. The older breadboard divider and
  relay descriptions in the firmware header are historical.
- [Firmware architecture, Rev 3](Firmware%20Architecture.pdf) and
  [RTOS architecture](RTOS%20Architecture.pdf) explain the design decisions
  and task layout at their respective revisions. Their editable ReportLab
  sources are in [docs/architecture](docs/architecture/) and
  [docs/rtos](docs/rtos/). With ReportLab and the Windows Segoe UI and
  Consolas fonts, run `python docs/architecture/build.py` or
  `python docs/rtos/build.py` from `Firmware/` to regenerate the references.
- [Firmware development and bench notes](docs/bench-notes.md) preserve
  the C migration and build records, sensor bring-up results, alarm and
  watchdog tests, and earlier BLE and power experiments. Start with the
  [lamp relay bring-up](docs/bench-notes.md#lamp-relay-bring-up-test_lamp-ahead-of-wp7)
  or the [on-device calibration record](docs/bench-notes.md#on-device-calibration-wp8-28-oct-2026-written-not-flashed)
  when investigating those parts of the system.

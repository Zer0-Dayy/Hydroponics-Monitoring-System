# Hydroponics Control desktop app

A Windows and Linux app for live readings, controller setup, calibration,
and local history. It uses Flet for the desktop interface and Bleak for
[BLE contract v2](FirmwareContract.md). Use the integrated
[ESP32-S3 firmware](../Firmware/README.md) for a real controller, or start
with the simulator to explore the workflows.

## Install and run

Use **Python 3.11 or newer**. The first desktop launch may download the Flet
viewer. For a live controller, use a computer with a BLE adapter. The demo
works without an adapter.

### Windows

Run in PowerShell from the repository root on Windows 10 or 11.

```powershell
cd BluetoothControlApp
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r Requirements.txt
.\.venv\Scripts\python.exe Main.py --demo
```

### Linux

Use a desktop session with BlueZ. On Ubuntu 24.04, install the desktop and
Bluetooth dependencies. [Ubuntu GTK package](https://launchpad.net/ubuntu/noble/%2Bpackage/libgtk-3-0t64)

```sh
sudo apt install python3-venv bluez libgtk-3-0t64 libgstreamer1.0-0 \
  libgstreamer-plugins-base1.0-0 libsecret-1-0
```

Then run from the repository root.

```sh
cd BluetoothControlApp
python3 -m venv .venv
.venv/bin/python -m pip install -r Requirements.txt
.venv/bin/python Main.py --demo
```

Remove `--demo` from the launch command for the live app.

## Walk through the demo

1. Open **Connection**, choose **Scan for controllers**, then choose **Connect**
   beside **Hydro-ESP1 (demo)**. The app opens **Dashboard**. Wait for telemetry
   and check the values, their age, the relay status, and the controller
   hardware panel.
2. On **Connection**, turn off **External 5 V present** and look at the
   Dashboard again. The simulator marks pH, TDS, and water distance unpowered
   while the 3.3 V measurements continue. Restore the switch to see recovery.
3. Enable **Over-temperature** in the demo controls. On **Dashboard**, check
   the latched alarm and the relay status. Clear the simulated condition,
   wait for a fresh reading, then use **Reset**. The simulator also lets you
   exercise reconnects with **Controller in Bluetooth range**.
4. Open **Metrics**, select **Hydro-ESP1 (demo)**, and [export a sample](#review-and-export-history).
   Demo readings are recorded under the simulator's own controller name.

The simulator exercises transitions and alarms that are disabled by default
in the hardware image. [Firmware power and safety policy](../Firmware/README.md#power-and-safety-policy)

## Connect to a controller

1. Power the ESP32-S3 and launch the app without `--demo`. On **Connection**,
   choose **Scan for controllers** and connect to **Hydro-ESP1**. Firmware
   0.11.0 uses an open BLE link. The app clears an older OS bond if one
   remains from a paired firmware version.
2. Read **Dashboard** first. Check the power mode and the age and health of
   each reading. The controller hardware panel polls about every 10 s while
   Dashboard is open and shows uptime, reset reason, CPU, RAM, and storage.
3. Open **Sensors** to confirm the fixed sensor list. Enable the devices that
   are fitted. A controller may refuse a change when an active or latched
   alarm depends on that sensor. Turning off light keeps automatic lamp
   control from energizing the relay.
4. Use **Calibration** for pH, TDS, and light. Check the results on Dashboard
   before relying on values for operation. [Probe calibration steps](#calibrate-a-probe)
5. To configure the controller's Wi-Fi station, open **Wi-Fi**, enter the
   network details, and select **Send to controller**. The SSID accepts up to
   32 UTF-8 bytes and the password must be 8–63 printable ASCII characters.
   A successful send confirms storage. Check the reported network status
   separately to see whether the controller joined.

The controller accepts one BLE client at a time. Use **Disconnect** before
switching computers. BLE carries the app session and all commissioning
commands. The open link lets nearby devices read telemetry or send
commands, and it carries Wi-Fi credentials unencrypted. The app clears
the password field when you send settings, and the controller stores the
credentials in flash. [BLE link and Wi-Fi command details](FirmwareContract.md)

## Calibrate a probe

Start with a current BLE connection, an enabled sensor, and fresh readings.
pH and TDS require external 5 V. For each reference, enter the measured or
labelled value and choose **Capture** after the sample settles.

| Sensor | Reference procedure | What changes |
| --- | --- | --- |
| pH-4502C | Choose two buffers at least 1 pH apart, such as 7.00 and 4.00. Rinse between buffers and let the probe settle for about a minute in each. Capture both points. | The controller fits a line for the probe. |
| SEN0244 TDS | Place the TDS and DS18B20 probes in a solution of known ppm. Wait for both readings to settle, then capture one point. | The controller fits a correction factor for the estimate compensated to 25 °C. |
| DFR0026 light | Place a lux meter beside the sensor under the same light. Capture once with the meter's current reading. | Telemetry changes from mV to lux. Lamp thresholds still use the ADC voltage. |

Select **Save calibration**. The controller checks the fit, saves it in
flash, and applies it on the next sample. For a rejected fit, check the
reference values, sensor power, and settling time before taking new
captures. **Reset to factory default** restores the firmware's defaults.
The **Dashboard** bands are indicative. Calibrated readings and physical
references are the basis for deciding operating limits.

## Review and export history

1. With the desktop app connected, wait for readings to arrive. Recording
   continues on every page while the BLE session is active.
2. Open **Metrics**. Pick the controller, select a time range, and choose
   the metrics to include. **Custom** accepts local times in
   `YYYY-MM-DD HH:MM` format.
3. Choose **Download CSV** or **Download JSON**, then pick a destination.
   Select **Export usable values only** to filter out samples with no
   numeric value.

The controller reports sample age. The app estimates the sample timestamp
by subtracting that age from the computer's receive time. Telemetry arrives
roughly every 3 s and is deduplicated by controller, boot, metric, and
sequence. Exports use UTC and preserve health and units. An unusable
sample has a blank CSV value or a JSON `null`. JSON also records the
export filters and available firmware, boot, and configuration metadata.

Latest, min, max, mean, and trends use the newest usable unit in their
time window. After light calibration, for example, the view can show lux
while earlier mV samples remain available in exports. Restrict the time
range when comparing readings under a specific calibration.

| System | Default database |
| --- | --- |
| Windows | `%LOCALAPPDATA%\HydroponicsControlApp\Readings.sqlite3` |
| Linux | `~/.local/share/HydroponicsControlApp/Readings.sqlite3`, or under `$XDG_DATA_HOME` if set |

Close the app before copying the database. Older v1 readings remain in
the original `Readings` table. The Metrics page uses v2 samples.

## Package the app

Run the build script from `BluetoothControlApp/` on the target operating
system. The scripts install `Requirements-build.txt` and bundle the
desktop client with PyInstaller.

| Platform | Build command | Output |
| --- | --- | --- |
| Linux | `./build_linux.sh` | `dist/HydroponicsControl` |
| Windows | `powershell -ExecutionPolicy Bypass -File .\build_windows.ps1` | `dist\HydroponicsControl.exe` |

Launch the output with `--demo` for a smoke test. Use `./build_linux.sh --install`
to also install the executable and application-menu launcher for the
current Linux user. The Linux build uses its own `.venv-linux` environment.
Build on the oldest Linux distribution you intend to support so the
executable targets a compatible glibc version.

## Develop and test

Run these commands from `BluetoothControlApp/` after setting up the virtual
environment. The browser preview is for UI development. Use the desktop
window for the native export dialog.

**Linux**

```sh
.venv/bin/python -m pip install -r Requirements-dev.txt
.venv/bin/python -m pytest tests
.venv/bin/python Main.py --demo --web 8551
```

**Windows**

```powershell
.\.venv\Scripts\python.exe -m pip install -r Requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest tests
.\.venv\Scripts\python.exe Main.py --demo --web 8551
```

Open the preview at `http://localhost:8551`. The test suites cover
framing, simulated reconnects, power and alarm cases, calibration,
history deduplication, and exports. A real BLE check needs a live board.

| Code | Responsibility |
| --- | --- |
| [Main.py](Main.py) and page modules | Navigation, dashboard, and commissioning workflows |
| [Session.py](Session.py), [BleLink.py](BleLink.py), and [OsBond.py](OsBond.py) | Connection, notifications, reconnect, commands, and stale OS bond cleanup |
| [Protocol.py](Protocol.py) and [Catalog.py](Catalog.py) | Framing, validation, and sensor and metric definitions |
| [Store.py](Store.py) and [Export.py](Export.py) | SQLite history and CSV/JSON export |
| [DemoController.py](DemoController.py) | In-process v2 simulator |
| [assets/](assets/) and [make_icon.py](tools/make_icon.py) | App artwork and icon regeneration with Pillow |

## Troubleshooting

| What you see | What to check |
| --- | --- |
| Scan returns no controller | Check that Bluetooth is enabled, the firmware is advertising, and another computer has released the BLE connection. Run `tools/ble_scan.py` with the app's Python environment to see whether the adapter discovers other devices. |
| The OS asks for pairing, or the controller asks for an authenticated link | Check the flashed firmware version and use the matching 0.11.0 image. |
| The app reconnects or State becomes stale | Bring the two devices closer together. The launch option `--log link.log` captures the link and reconnect decisions. |
| External 5 V is lost | Check the controller's power mode and supply before using pH, TDS, or distance readings. The default firmware reports the loss while leaving acquisition enabled. [Firmware power policy](../Firmware/README.md#power-and-safety-policy) |
| TDS calibration is blocked | Check that the DS18B20 is enabled and supplying fresh water temperature, then capture in the reference solution again. |

# Hydroponics Control

Desktop app for the ESP32 hydroponics controller, native on **Windows (.exe) and Linux**. It talks
[contract v2](FirmwareContract.md) to the [production firmware](../Firmware/README.md) over
**Bluetooth LE only**: data and settings go over an open Bluetooth link (no passkey, no pairing). The controller's own Wi-Fi is
only a setting on the Wi-Fi page (for future server publishing); the app never connects over Wi-Fi.

There is no server. The app keeps its own history and exports it. Run with `--demo` to use a built-in
simulator; no hardware is needed.

## Pages

- **Dashboard**: live values for the 10 metrics (BME280 air temperature, humidity and pressure;
  DS18B20 water temperature; light; water distance; pH; TDS; TVOC; CO2eq).
  - Each card shows how old the reading is, an indicative band and a 10-minute sparkline. A status label
    appears only when something needs attention (Warming up, Stale, Fault, Unpowered, Disabled…); normal
    readings, calibrated or not, carry no label.
  - Summary cards show the Bluetooth link, the power mode, the grow lamp (on/off and why), alarms and the
    number of samples recorded.
  - **Controller hardware** (firmware 0.10.0+, `get_diagnostics` every 10 s while the Dashboard is open):
    uptime and the cause of the last restart (a brownout or crash is shown in red), CPU load per core with a
    60 s peak, RAM use and storage (PSRAM, NVS entries, app partition).
  - A latched alarm shows a banner with **Reset**. The reset is refused while the condition persists.
- **Connection**: Bluetooth scan, then **Connect**: no passkey, no pairing. Automatic reconnect after a
  link loss, and Disconnect.
- **Wi-Fi**: the controller's network status and sending SSID and password over Bluetooth. With full power
  transitions enabled in firmware, the controller switches Wi-Fi off until external power returns.
- **Metrics**: the history recorded on this computer.
  - Filters: controller, time range (1 h, 24 h, 7 days, all, custom) and metrics.
  - A summary table (latest, min, max, mean, sample count, % usable) and a trend chart.
  - **Download CSV** and **Download JSON**, with UTC timestamps. The JSON adds a `meta` block: node, firmware,
    boot ids, configuration revisions and the filters used.
- **Sensors**: the board's fixed sensors, with live health and an on/off switch.
  - The controller saves the change and confirms it once applied.
  - It refuses to switch off a sensor whose alarm is latched or active.
  - Side effects are shown before you confirm, for example "switching off light keeps the lamp OFF".
- **Calibration**: on-demand calibration of the sensors that need it, stored on the controller (see below).

The status bar always shows the controller, the Bluetooth link, whether state is stale, the power mode and
alarms. The production build reports **Normal power** and **Low-power mode** from the external 5 V sense
pin. With full transitions enabled, low-power mode pauses pH, TDS, water level, and Wi-Fi, and keeps the lamp
relay off. Bluetooth and the 3.3 V sensors keep running. The app also shows **Switching…** during a transition
and **Power transition degraded** when a step times out. See the [firmware power policy](../Firmware/README.md#low-power-mode-wp9-28-oct-2026-written-not-flashed).

### Calibration

Only the sensors whose reading depends on the individual probe are calibrated, and only when needed (new
probe, drift, or periodically for pH):

| Sensor | Procedure | Result |
| --- | --- | --- |
| pH-4502C | Two buffers at least 1 pH apart (e.g. 7.00 then 4.00): rinse, immerse, let it settle ~1 min, **Capture** each | pH line, shown as **OK** instead of Uncalibrated |
| SEN0244 TDS | One reference solution of known ppm, with the water temperature probe in it, **Capture** | K-factor |
| DFR0026 light | A lux meter next to the sensor, type its reading and **Capture** at the same moment | value in **lux** instead of mV |

**Save calibration** sends the captured values with the reference values. The controller checks that the fit
is plausible (`bad_fit` otherwise), stores it in its flash, where it survives reboots and firmware updates,
and applies it from the next sample. **Reset to factory default** goes back to the firmware's default. The
lamp thresholds do not move when the light sensor is calibrated. Not calibrated here: BME280 and DS18B20
(factory-calibrated), SGP30 (learns its own baseline), HC-SR04 (reports distance).

### How data is recorded

- Every telemetry frame is recorded, whatever page is open.
- The controller has no clock. Sample time is the receive time minus the age the controller reports.
- The firmware samples every 1 s and sends a frame every 3 s, so **history resolution is about 3 s**.
- Each sample is stored once per (controller, boot, metric, sequence number), although frames repeat it.
- Samples without a usable value are kept with their health status. They export as an empty value (CSV)
  or `null` (JSON).
- Storage is SQLite in the user data folder:
  - Windows: `%LOCALAPPDATA%\HydroponicsControlApp\Readings.sqlite3`
  - Linux: `~/.local/share/HydroponicsControlApp/Readings.sqlite3`

  The v1 `Readings` table remains in the database. Metrics displays readings recorded by v2.
- Wi-Fi passwords are never stored by the app.

## Run on Windows

Needs Python 3.10+ and a Bluetooth LE adapter (Windows 10 or 11).

~~~powershell
cd BluetoothControlApp
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r Requirements.txt
.\.venv\Scripts\python.exe Main.py --demo
~~~

Leave out `--demo` to use the real controller. The first start downloads the Flet desktop client once.

**Connecting:** open **Connection**, choose **Scan for controllers**, then **Connect** next to
`Hydro-ESP1`. There is no passkey and nothing to pair (firmware 0.11.0+).

If Windows still holds a pairing from older firmware, the app removes it before connecting (`OsBond.py`),
so you never have to remove the device in Windows Settings. It also reads the GATT services uncached, so a
table Windows cached from an older firmware is not reused.

**Build the .exe:**

~~~powershell
powershell -ExecutionPolicy Bypass -File .\build_windows.ps1
~~~

The result is `dist\HydroponicsControl.exe`, one file with the plant icon (window, taskbar and file). It
installs `Requirements-build.txt` (PyInstaller) into `.venv` first. Check it with
`.\dist\HydroponicsControl.exe --demo`.

## Run on Linux (Ubuntu)

Needs Python 3.10+, BlueZ, a display session and the GTK runtime the Flet client uses:

~~~bash
sudo apt install python3-venv libgtk-3-0 libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libsecret-1-0 bluez
~~~

~~~bash
cd BluetoothControlApp
python3 -m venv .venv
.venv/bin/python -m pip install -r Requirements.txt
.venv/bin/python Main.py --demo
~~~

Connecting works the same way. A BlueZ pairing left from older firmware is removed first (`OsBond.py`).

**Build the native Linux binary and launcher:**

~~~bash
./build_linux.sh --install
~~~

It builds `dist/HydroponicsControl` (one file, x86_64) in its own `.venv-linux`, then `InstallDesktop.py`
installs it with the plant icon and a GNOME launcher for the current user. PyInstaller does not
cross-compile: build on Linux (or WSL on a Windows PC), on the oldest distribution you want to run it on.
Without `python3-venv` the script installs into a pip-less venv with your user pip instead.

## One computer at a time

The controller serves **one** computer at a time, but any computer can connect. It is not tied to an
owner. To move to another computer, choose **Disconnect** (or close the app) on the first one. The ESP
advertises again at once and shows up in the other computer's scan.

- If the first computer just vanished (closed lid, out of range), the ESP frees itself within about 4 s.
- A connection that never subscribes (an operating system holding the link with no app on it) is dropped
  after 20 s.

There is no BOOT-button step and no reset needed.

**Trade-off:** the link is not encrypted. Anyone within Bluetooth range can read the readings and send
commands. That includes the Wi-Fi password sent from the Wi-Fi page, which travels in clear.

## Development

Run the checks from `BluetoothControlApp/` after creating the app's virtual environment.

### Linux

~~~bash
.venv/bin/python -m pip install -r Requirements-dev.txt
.venv/bin/python -m pytest tests
.venv/bin/python Main.py --demo --web 8551
~~~

### Windows

~~~powershell
.\.venv\Scripts\python.exe -m pip install -r Requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest tests
.\.venv\Scripts\python.exe Main.py --demo --web 8551
~~~

Open `http://localhost:8551` for the browser preview.

- **Tests:** they cover the protocol decoder and power modes, store dedupe and sample times, CSV/JSON export,
  and the session against the demo controller (Bluetooth reconnect, connect with no passkey that clears a stale OS pairing, pH / TDS / light
  calibration and refused fits, low-power mode, and the refused disable while an alarm is latched).
- **Demo switches:** in demo mode, the Connection page has switches for Bluetooth going out of range, the
  external 5 V (low-power mode), a controller Wi-Fi outage and an over-temperature alarm.
- **Icons:** `python tools/make_icon.py` (needs Pillow) rebuilds `assets/AppIcon*` from
  `assets/source-icon.webp`, with the background made transparent.

### Code map

| File | Role |
| --- | --- |
| `Main.py` | Window shell: status bar, navigation, page routing |
| `DashboardPage.py`, `MetricsPage.py`, `SensorsPage.py`, `CalibrationPage.py`, `ConnectionPage.py`, `WifiPage.py` | One module per page |
| `Theme.py`, `Charts.py` | Shared styling; canvas sparkline and trend chart |
| `Session.py` | Bluetooth link and reconnect, recording, commands and calibration (no UI) |
| `BleLink.py`, `Links.py` | Bluetooth (bleak) link |
| `OsBond.py` | Removes an OS pairing left by firmware 0.10 or older (WinRT / BlueZ) |
| `Protocol.py`, `Catalog.py`, `Meter.py` | Contract v2 framing and validation; metric labels; display bands |
| `Store.py`, `Export.py` | SQLite history; CSV and JSON export |
| `DemoController.py` | v2 simulator for `--demo` |
| `assets/`, `tools/make_icon.py` | App icons and the script that makes them |
| `build_windows.ps1`, `build_linux.sh`, `InstallDesktop.py` | Native builds; Linux launcher |

## Limits

- After a lux calibration of the light sensor, recorded history holds mV samples before and lux samples after
  (the Unit column tells them apart). The Metrics summary, chart and Dashboard sparkline do not separate them:
  pick a time range after the calibration.
- Indicative bands are not crop advice. Set them for the real crop and calibrated sensors (`Meter.py`).
- The app reports what the controller says. It cannot prove the radio link's security properties.
  Production hardware also needs Secure Boot, Flash Encryption and a commissioning policy.

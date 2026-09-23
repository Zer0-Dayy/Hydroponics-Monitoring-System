# Hydroponics Control

An Ubuntu desktop prototype for the ESP32 hydroponics controller. The GUI uses Flet and Bleak/BlueZ. The matching demo firmware and terminal build instructions are in [ESP-Demo](../ESP-Demo/BuildGuide.md). Run with `--demo` to review the screens without hardware.

## What it does

- Scan for the project BLE service, enter the ESP serial passkey in the app for first pairing, and connect through BlueZ.
- Receive controller mode and WiFi state on every page. Dashboard subscribes to sensor telemetry only while it is open.
- Timestamp displayed BLE readings on the laptop and save them in local SQLite under `XDG_DATA_HOME/HydroponicsControlApp/Readings.sqlite3`, or `~/.local/share/HydroponicsControlApp/Readings.sqlite3`.
- Send WiFi SSID and password from a form. The app does not save the password and clears the field after sending. The ESP demo stores it in NVS.
- Add or remove supported devices from a fixed catalog. The ESP persists the configured list and simulated readings follow it.
- In low-power mode, gray out the WiFi page and nonessential sensor/device controls. Reservoir level, pH, and TDS remain active. A stale controller state pauses configuration.

The ESP demo reports simulated device presence and simulated values; it has no attached sensors. WiFi acknowledgement means settings were saved, while the state heartbeat reports connection progress. Readings are logged only while Dashboard is open. Previously saved readings remain visible in local history.

## Run on Ubuntu

Requires Python 3.10+, BlueZ, a working Bluetooth adapter, and a display session.

~~~bash
cd BluetoothControlApp
python3 -m venv .venv
.venv/bin/pip install -r Requirements.txt
.venv/bin/python Main.py --demo
~~~

Omit `--demo` to connect to the ESP32. For first pairing, connect a local serial monitor to the ESP, reset it, and enter the six-digit passkey printed there in the app's Connect page. The app provides a temporary BlueZ pairing agent, so a separate Ubuntu Settings prompt is unnecessary. The first authenticated laptop becomes the demo controller's owner. See the firmware [BuildGuide](../ESP-Demo/BuildGuide.md) for flashing and reset details.

## Build an Ubuntu executable

Run on the Ubuntu laptop that will use the app:

~~~bash
.venv/bin/pip install pyinstaller
.venv/bin/flet pack Main.py --name HydroponicsControl
~~~

Distribute the `dist/HydroponicsControl` executable produced on Ubuntu. The executable still needs Ubuntu's Bluetooth/BlueZ service and a display session. The Ubuntu executable packaging command completed successfully in this development environment; the bundled app still needs a live display and Bluetooth adapter for an interactive check.

## Architecture and limits

Flet suits a small Python commissioning app with async Bleak handlers. PySide6 is a reasonable alternative if a native desktop UI or richer charts become central. The protocol and GATT map are documented in [FirmwareContract.md](FirmwareContract.md). The old handover WiFi/MQTT source is reference material, not the BLE firmware used here.

The demo firmware uses authenticated BLE pairing, bonding, an owner lock, and protected attributes. The application cannot independently prove the radio link's security properties or guarantee immunity from external attacks. This demo stores WiFi credentials in ordinary NVS; production hardware needs Secure Boot, Flash Encryption, port protection, and a recovery/commissioning policy. Real boot, pairing, button, and WiFi behavior still require tests on an ESP32 board.

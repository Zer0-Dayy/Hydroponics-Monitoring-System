# Hydroponics Control

A first-milestone Ubuntu desktop prototype for the redesigned ESP32-S3 controller. The GUI is built with Flet 1.0 and uses Bleak/BlueZ for BLE. Run with --demo to review the screens without hardware. Live BLE requires the firmware in FirmwareContract.md; the inherited WiFi/MQTT firmware does not expose it.

## Implemented

- Scan for controllers advertising the project service, select one, pair through Ubuntu, and connect.
- Show live state and sensor data. Each BLE reading is timestamped on the laptop and saved in local SQLite under XDG_DATA_HOME/HydroponicsControlApp/Readings.sqlite3, or ~/.local/share/HydroponicsControlApp/Readings.sqlite3.
- Send a WiFi SSID/password from a form. The password is never saved by the app and the field clears after sending.
- Show configured devices and add/remove them from a fixed catalog. The ESP validates device types and owns the GPIO map.
- In low-power mode, disable and gray the WiFi form and nonessential sensor/device controls. The default policy keeps reservoir level, pH and TDS available; firmware state may override this list.
- Pause configuration if the controller state has not arrived for 15 seconds.

The device list is the ESP's configured list. Physical connection is shown as Online only when firmware explicitly reports it. A configuration entry alone cannot prove a sensor is plugged in. WiFi acknowledgement means settings were persisted, not that WiFi has connected.

## Run on Ubuntu

Requires Python 3.10+, BlueZ 5.55+, and a working Bluetooth adapter.

~~~bash
cd HydroponicsControlApp
python3 -m venv .venv
.venv/bin/pip install -r Requirements.txt
.venv/bin/python Main.py --demo
~~~

Omit --demo to scan for a real ESP32-S3. If Ubuntu pairing asks for a code, use the unique code supplied with the physical controller and start its physical commissioning window.

## Build an Ubuntu executable

Run on the Ubuntu machine that will use the app:

~~~bash
.venv/bin/pip install pyinstaller
.venv/bin/flet pack Main.py --name HydroponicsControl --onedir
~~~

Distribute the full dist/HydroponicsControl folder. The binary still needs Ubuntu's system Bluetooth/BlueZ service and a display session. Build the Ubuntu executable on the target laptop using the command above. The target laptop and ESP hardware remain untested.

## Design choices and source notes

Flet suits a small Python-first commissioning app because async handlers can await Bleak without blocking the UI. PySide6 is a reasonable alternative for a deeply native UI or complex charting; the redesign specification itself suggests PySide6/Bleak. Bleak remains the preferred Python GATT client here.

The current source is reference material, not firmware for this app. The handover documents the old MQTT/HTTP design and ESP1 pin assignments. The redesign specification calls for BLE commissioning and leaves UUIDs and packet format open. Its revision decision retains pH/TDS; older requirement diagrams still mention EC. The inherited Temperature_Humidity_DHT21 profile name is a compatibility reference even though the tested physical sensor is DHT22.

Before live deployment, the firmware team must implement FirmwareContract.md, provision each unit with an authenticated pairing method, enforce encrypted authenticated GATT permissions, add physical commissioning control, validate and persist device changes atomically, and report actual sensor presence and low-power state. Bleak cannot independently establish that the firmware enforced MITM protection.

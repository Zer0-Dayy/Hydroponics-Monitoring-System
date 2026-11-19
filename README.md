# Hydroponics Monitoring System

The ESP32-S3 controller measures reservoir and air conditions, monitors alarms, and controls a grow-lamp relay. The Windows and Linux desktop app connects over Bluetooth Low Energy for live readings, local history, commissioning, and calibration.

- [PCB design](PCB%20Design/) and [hardware documentation](Documentation/)
- [Firmware guide](Firmware/README.md)
- [Desktop app guide](BluetoothControlApp/Readme.md) and [BLE contract](BluetoothControlApp/FirmwareContract.md)
- [Original project handover](Project%20Handover/Source%20Code/HANDOVER.md)

## Firmware

Install PlatformIO and a host C compiler for the native tests. Run from the repository root.

```sh
cd Firmware
pio run -e esp32dev
pio test -e native
```

## Bluetooth Control App

On Linux, follow the app guide for desktop and Bluetooth prerequisites, then run these commands from the repository root.

```sh
cd BluetoothControlApp
python3 -m venv .venv
.venv/bin/python -m pip install -r Requirements.txt
.venv/bin/python Main.py --demo
```

The demo uses simulated readings. Run `.venv/bin/python Main.py` to connect to the controller. The firmware uses an unencrypted BLE link, including when sending Wi-Fi credentials.

From `BluetoothControlApp/`, install the test dependencies, run the tests, and build the Linux package.

```sh
.venv/bin/python -m pip install -r Requirements-dev.txt
.venv/bin/python -m pytest tests
./build_linux.sh
```

For Windows setup and testing, see the app guide. From `BluetoothControlApp/` on Windows, package the app with `powershell -ExecutionPolicy Bypass -File .\build_windows.ps1`.

The upstream commit, attribution, and license status are recorded in [PROVENANCE.md](PROVENANCE.md).

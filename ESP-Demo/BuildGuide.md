# ESP32-S3 demo: terminal build and flash

This firmware targets an ESP32-S3 DevKitC-1 and matches the BluetoothControlApp GATT service. It has no sensor drivers or physical relays. The BOOT button is GPIO 0 on the target board; press it after startup to toggle the simulated low-power mode.

## Build tools

Install Python 3.10+ and PlatformIO Core on Ubuntu, then build from this folder:

~~~bash
python3 -m venv .venv
.venv/bin/pip install platformio==6.2.0
.venv/bin/pio run
~~~

The first build downloads the Espressif32 platform, Xtensa toolchain, Arduino core, NimBLE-Arduino, and ArduinoJson. The exact board and library versions are pinned in platformio.ini.

Connect the ESP32-S3 by USB and identify its port, usually /dev/ttyACM0 or /dev/ttyUSB0:

~~~bash
.venv/bin/pio device list
.venv/bin/pio run -t upload --upload-port /dev/ttyACM0
.venv/bin/pio device monitor -p /dev/ttyACM0 -b 115200
~~~

If the upload cannot open the port on Ubuntu, add your user to the dialout group and sign out/in. Some boards need BOOT held while pressing RESET to enter flashing mode. Use the board's actual port in the commands.

## Pair and try the app

1. Open the serial monitor and reset the ESP. It prints a fresh six-digit BLE passkey. Keep the serial monitor local to the technician.
2. Run the Python app on the Ubuntu laptop, open Connect, scan, and choose Hydroponics-ESP1. Ubuntu/BlueZ should ask for the displayed passkey on first pairing. Only the first authenticated, bonded laptop becomes the owner.
3. Open Dashboard. Simulated readings appear for the configured sensors. Open Sensors & devices to add or remove supported types; return to Dashboard to see the new set. Sensor readings are generated only while Dashboard subscribes.
4. Enter the WiFi SSID and WPA password on WiFi configuration. The ESP stores them in NVS, attempts a WiFi connection, and sends updated WiFi state over BLE. A configuration acknowledgement means saved, not yet connected.
5. Press BOOT after the ESP has started. The ESP turns WiFi off and reports low_power. The dashboard and WiFi page update from the continuous state heartbeat. Only level, pH, and TDS keep producing demo data. Press BOOT again to restore normal mode and reconnect WiFi.

The configured device list and WiFi credentials survive reboot. To reset the demo's first-owner bond and saved settings, erase flash deliberately with PlatformIO's erase target, then reflash. Erasing flash destroys the demo configuration and bonds.

## Security boundary

The BLE server requests bonding, MITM-authenticated LE Secure Connections, 16-byte encryption keys, authenticated characteristic access, and a passkey generated at boot. It permits one owner identity and one connected client. Commands are rejected unless the link is encrypted, authenticated, and bonded. The passkey is available only through local serial monitoring; it is not advertised.

This is a **demo**, not a claim of immunity from every external attack. Arduino Preferences uses ordinary NVS here; someone with physical flash access may recover stored WiFi credentials. Production hardware should enable ESP32-S3 Secure Boot and Flash Encryption, protect the service port, define bond recovery, and test BLE security with a real laptop. The passkey method also requires the technician to have local serial access for first pairing.

## Hardware limits

Compilation verifies source compatibility with the pinned toolchain. Boot, pairing, WiFi connection, and button behavior require a physical ESP32-S3 and cannot be confirmed by a compiler. If the board is a different ESP32 variant, change the board target and BOOT pin only after checking that board's pinout and BLE support.

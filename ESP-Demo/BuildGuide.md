# ESP32 demo: terminal build and flash

This firmware defaults to the original ESP32 Dev Module (`esp32dev`) and matches the BluetoothControlApp GATT service. An ESP32-S3 DevKitC-1 build remains available as an alternate environment. It has no sensor drivers or physical relays. The BOOT button is GPIO 0 on these development boards; press it after startup to toggle the simulated low-power mode.

## Build tools

Install Python 3.10+ and PlatformIO Core on Ubuntu, then build from this folder:

~~~bash
python3 -m venv .venv
.venv/bin/pip install platformio==6.2.0
.venv/bin/pio run -e esp32dev
~~~

The first build downloads the Espressif32 platform, Xtensa toolchain, Arduino core, NimBLE-Arduino, and ArduinoJson. The platform and library versions are pinned in `platformio.ini`. `esp32dev` is the default environment; use `-e esp32-s3-devkitc-1` only for an actual ESP32-S3 board. An S3 binary cannot be flashed to an original ESP32.

Connect the ESP32 by USB and identify its port, often `/dev/ttyUSB0` or `/dev/ttyACM0`:

~~~bash
.venv/bin/pio device list
.venv/bin/pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
.venv/bin/pio device monitor -p /dev/ttyUSB0 -b 115200
~~~

If the upload cannot open the port on Ubuntu, add your user to the dialout group and sign out/in. Some boards need BOOT held while pressing RESET to enter flashing mode. Use the board's actual port in the commands. If you previously built the S3 environment, the explicit `-e esp32dev` selects the correct ESP32 binary. PlatformIO keeps each environment's build output separate.

## Pair and try the app

1. Open the serial monitor and reset the ESP. It prints a fresh six-digit BLE passkey. Keep the serial monitor local to the technician.
2. Run the Python app on the Ubuntu laptop, open Connect, scan, enter the passkey in the app, and choose Hydroponics-ESP1. The app registers a temporary BlueZ keyboard agent for first pairing, so Ubuntu Settings does not need to display a prompt. On later connections from the same bonded laptop, the code can be left blank. Only the first authenticated, bonded laptop becomes the owner.
3. Open Dashboard. Simulated readings appear for the configured sensors. Open Sensors & devices to add or remove supported types; return to Dashboard to see the new set. Sensor readings are generated only while Dashboard subscribes.
4. Enter the WiFi SSID and WPA password on WiFi configuration. The ESP stores them in NVS, attempts a WiFi connection, and sends updated WiFi state over BLE. A configuration acknowledgement means saved, not yet connected.
5. Press BOOT after the ESP has started. The ESP turns WiFi off and reports low_power. The dashboard and WiFi page update from the continuous state heartbeat. Only level, pH, and TDS keep producing demo data. Press BOOT again to restore normal mode and reconnect WiFi.

The configured device list and WiFi credentials survive reboot. If Ubuntu has a stale pairing after reflashing or erasing the ESP, remove the old device in Ubuntu Bluetooth Settings, then scan and pair again in the app. To reset the demo's first-owner bond and saved settings, erase flash deliberately with PlatformIO's erase target, then reflash. Erasing flash destroys the demo configuration and bonds. The serial monitor now prints authentication flags and disconnect reasons when a pairing attempt fails.

## Security boundary

The BLE server requests bonding, MITM-authenticated LE Secure Connections, 16-byte encryption keys, authenticated characteristic access, and a passkey generated at boot. It permits one owner identity and one connected client. Commands are rejected unless the link is encrypted, authenticated, and bonded. The passkey is available only through local serial monitoring; it is not advertised.

This is a **demo**, not a claim of immunity from every external attack. Arduino Preferences uses ordinary NVS here; someone with physical flash access may recover stored WiFi credentials. Production hardware should enable ESP32 Secure Boot and Flash Encryption, protect the service port, define bond recovery, and test BLE security with a real laptop. The passkey method also requires the technician to have local serial access for first pairing.

## Hardware limits

Compilation verifies source compatibility with the pinned toolchain. Boot, pairing, WiFi connection, and button behavior require a physical ESP32 and cannot be confirmed by a compiler. If the actual board has unusual flash or pin wiring, identify its exact model before overriding the `esp32dev` board settings.

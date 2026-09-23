# ESP32 BLE contract, version 1

The [ESP demo firmware](../ESP-Demo/BuildGuide.md) implements this contract for simulated sensors. The older WiFi/MQTT firmware does not. The GATT map and message format were selected for this prototype because the redesign handover left them open.

## GATT service

| Purpose | UUID | Access |
| --- | --- | --- |
| Hydroponics service | 98a90f00-346e-4e65-9cbd-7687685bdc01 | Advertise |
| State | 98a90f01-346e-4e65-9cbd-7687685bdc01 | Authenticated read and notify |
| Telemetry | 98a90f02-346e-4e65-9cbd-7687685bdc01 | Authenticated notify |
| Command | 98a90f03-346e-4e65-9cbd-7687685bdc01 | Authenticated write with response |
| Result | 98a90f04-346e-4e65-9cbd-7687685bdc01 | Authenticated notify |
| Auth gate | 98a90f05-346e-4e65-9cbd-7687685bdc01 | Authenticated read, value `HYDRO_SECURE_V1` |

The firmware requests LE Secure Connections with bonding, MITM authentication, a boot-generated six-digit passkey displayed on local serial, authenticated attribute access, and a single bonded owner. The auth gate is an early protected read, not independent cryptographic proof. The app supplies a temporary BlueZ `KeyboardOnly` agent for passkey entry, pairs on that agent's D-Bus connection, then lets Bleak discover GATT services. The first authenticated owner persists until flash is erased. This demo does not implement a time-limited commissioning window.

## Framing and traffic

Each State, Telemetry, Command, and Result message is UTF-8 JSON prefixed with a two-byte little-endian payload length. Maximum JSON payload: 4096 bytes. BLE packets may split a frame; both sides reassemble it. The app sends ordered 20-byte GATT writes with response. The ESP rejects invalid length and unsupported operations, then validates fields before changing stored configuration. It never echoes WiFi credentials in results.

State is read at connection and notified every two seconds on every page, with an immediate notification when the BOOT button changes mode. The app treats state as stale after 15 seconds. Dashboard alone subscribes to Telemetry. Sensors & devices requests the configured list when opened; other pages do not receive it repeatedly. Sensor telemetry is emitted every three seconds only for configured, active sensor types. The ESP does not stream saved history; the laptop records received readings locally.

State example:

~~~json
{"v":1,"node":"ESP1 Demo","mode":"low_power","wifi":"disabled","battery_percent":74,"active_types":["level","ph","tds"]}
~~~

Telemetry example:

~~~json
{"v":1,"node":"ESP1 Demo","device":"esp1-ph","metric":"Solution pH","value":6.1,"unit":"pH"}
~~~

Command example:

~~~json
{"v":1,"id":"9fb8ab0e-7375-4075-b078-bca8456c8014","op":"set_wifi","data":{"ssid":"Greenhouse","password":"example-secret"}}
~~~

Supported operations are `get_devices`, `set_wifi`, `add_device`, and `delete_device`. The app sends a catalog type and label for add; the demo accepts one of each supported type and assigns its own ID. Deletion uses that ID. `get_devices` returns ID, type, label, and `online`. In this demo, `online` means simulated presence only.

Each command receives a Result notification with the same ID:

~~~json
{"v":1,"id":"9fb8ab0e-7375-4075-b078-bca8456c8014","status":"ok","data":{"accepted":true}}
~~~

Failure uses `status=error` and a short error string. `set_wifi` success means the settings were persisted, while later State messages report WiFi status. Device changes are serialized and persisted before success is returned.

## Low-power policy and limits

BOOT GPIO 0 toggles demo low-power mode. WiFi is disabled and climate, light, gas, and relays are paused. Level, pH, and TDS continue to produce simulated readings when Dashboard is open. State notifications continue in both modes. The simulated battery percentage is fixed at 74 and is not an actual measurement. This is an awake low-power simulation, not ESP deep sleep.

The firmware has no physical sensor or relay drivers and no ESP32 pin map for them. A production revision needs physical sensor presence checks, safe actuation behavior, hardware security features, a recovery process for a lost owner laptop, and real-board BLE/WiFi tests.

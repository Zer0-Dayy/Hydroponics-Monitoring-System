# Provisional ESP32-S3 BLE contract, version 1

The repository redesign specification leaves the exact UUID map and packet format open. This contract must be implemented and jointly tested in ESP32-S3 firmware before live use.

## GATT service

| Purpose | UUID | Access |
| --- | --- | --- |
| Hydroponics service | 98a90f00-346e-4e65-9cbd-7687685bdc01 | Advertise |
| State | 98a90f01-346e-4e65-9cbd-7687685bdc01 | Read + Notify |
| Telemetry | 98a90f02-346e-4e65-9cbd-7687685bdc01 | Notify |
| Command | 98a90f03-346e-4e65-9cbd-7687685bdc01 | Write with response |
| Result | 98a90f04-346e-4e65-9cbd-7687685bdc01 | Notify |
| Auth gate | 98a90f05-346e-4e65-9cbd-7687685bdc01 | Protected read, value HYDRO_SECURE_V1 |

All attributes require an encrypted authenticated connection, MITM protection and bonding. The auth gate is an early protected read, not independent cryptographic proof of link security. The ESP must enforce GATT permissions. Prefer LE Secure Connections with a unique device passkey displayed or provided out of band. Require a physical button or bounded commissioning window before new bonding or privileged writes. The app invokes Ubuntu/BlueZ pairing through Bleak pair=True.

## Framing

Every State, Telemetry, Command and Result message is UTF-8 JSON prefixed with a two-byte little-endian payload length. Maximum JSON payload: 4096 bytes. Notifications may split frames; the receiver reassembles them. The app sends commands in ordered 20-byte GATT writes with response. Firmware reassembles and rejects invalid length, malformed JSON, unsupported version, oversize message and requests outside the authenticated commissioning window. Never echo a secret in results or diagnostics.

## Message examples

State is read once after connection and notified at least every five seconds, including in low-power mode:

~~~json
{"v":1,"node":"ESP1","mode":"low_power","wifi":"disabled","battery_percent":74,"active_types":["level","ph","tds"]}
~~~

Normal mode is "normal". The laptop pauses configuration when state is older than 15 seconds. Notify mode changes immediately. active_types lists types usable in low-power mode.

Telemetry is notified when a reading is available. Do not publish a fake fresh value for a paused or failed sensor:

~~~json
{"v":1,"node":"ESP1","device":"ph-01","metric":"Solution pH","value":6.1,"unit":"pH"}
~~~

A command has a correlation ID, operation and structured data:

~~~json
{"v":1,"id":"9fb8ab0e-7375-4075-b078-bca8456c8014","op":"set_wifi","data":{"ssid":"Greenhouse","password":"example-secret"}}
~~~

Supported operations: get_devices, set_wifi, add_device, delete_device. add_device supplies type and label; the ESP chooses and validates pins from its hardware map. delete_device supplies the ESP-issued id. get_devices has empty data and returns an array of objects with id, type, label and online. If physical presence is unknown, report online=false or a separate presence state.

Every command returns one Result notification with the same ID:

~~~json
{"v":1,"id":"9fb8ab0e-7375-4075-b078-bca8456c8014","status":"ok","data":{"accepted":true}}
~~~

Failure uses status=error and a short error string without credentials. Success for set_wifi means validated settings were persisted; connection status appears later in State. Device changes must be serialized, checked for type support, duplicates, GPIO conflicts, initialization and persistence, then committed or rolled back. A successful result means committed. BLE callbacks should enqueue requests rather than mutate active device objects directly.

## Low-power policy

WiFi is disabled. Keep BLE state heartbeats and essential telemetry available. For this UI, the default essential set is reservoir level, pH and TDS; air climate, ambient light, MQ-7 and relay configuration are paused. The final firmware and power budget review must define sampling and safety behavior. The ESP rejects changes to unavailable devices and active_types drives the UI.

## Current limits

The inherited firmware has no BLE GATT server, so live operation cannot be verified yet. Firmware must supply a supported type/capability map for the final PCB; the UI catalog currently reflects the handover's known classes. SQLite stores laptop receipt times; ESP timestamps and historical synchronization are outside this milestone.

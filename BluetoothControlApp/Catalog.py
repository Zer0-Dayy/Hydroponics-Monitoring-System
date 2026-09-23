"""Predetermined ESP1 hardware choices from the project handover."""

from dataclasses import dataclass


@dataclass(frozen=True)
class DeviceType:
    key: str
    label: str
    profile: str
    gpios: tuple[int, ...]
    unit: str
    essential: bool = False
    kind: str = "sensor"


# GPIOs below describe the inherited ESP1 breadboard only. The new ESP32
# firmware owns its pin map; the app sends type identifiers, never GPIO numbers.
TYPES = (
    DeviceType("level", "Reservoir level", "Ultra_Sonic_Sensor", (23, 22), "cm", True),
    DeviceType("ph", "Solution pH", "Ph_sensor", (35,), "pH", True),
    DeviceType("tds", "Nutrient strength", "TDS_sensor", (34,), "ppm", True),
    DeviceType("climate", "Air temperature", "Temperature_Humidity_DHT21", (4,), "°C"),
    DeviceType("light", "Ambient light", "Analog_Light", (36,), "raw"),
    DeviceType("gas", "CO sensor", "MQ_7", (32, 33), "raw"),
    DeviceType("relay1", "Relay 1", "Relay_Actuator", (21,), "on/off", kind="actuator"),
    DeviceType("relay2", "Relay 2", "Relay_Actuator", (18,), "on/off", kind="actuator"),
    DeviceType("relay3", "Relay 3", "Relay_Actuator", (5,), "on/off", kind="actuator"),
    DeviceType("relay4", "Relay 4", "Relay_Actuator", (27,), "on/off", kind="actuator"),
)

BY_KEY = {item.key: item for item in TYPES}

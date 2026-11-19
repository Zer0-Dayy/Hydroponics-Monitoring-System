"""Display metadata for the firmware's fixed sensors and metrics (contract v2).

The live sensor list and enabled state come from the controller
(`get_sensors` and the State `enabled` list). This table only adds labels,
icons and number formatting, keyed by the protocol's metric keys.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class MetricInfo:
    key: str
    label: str
    sensor: str
    icon: str        # flet Icons name
    decimals: int


@dataclass(frozen=True)
class SensorInfo:
    key: str
    label: str
    metrics: tuple[str, ...]
    depends_note: str = ""


METRICS = (
    MetricInfo("air_temperature", "Air temperature", "bme280", "THERMOSTAT_ROUNDED", 1),
    MetricInfo("air_humidity", "Air humidity", "bme280", "WATER_ROUNDED", 0),
    MetricInfo("air_pressure", "Air pressure", "bme280", "SPEED_ROUNDED", 0),
    MetricInfo("water_temperature", "Water temperature", "ds18b20", "DEVICE_THERMOSTAT_ROUNDED", 1),
    MetricInfo("light_level", "Light level", "light", "WB_SUNNY_ROUNDED", 0),
    MetricInfo("water_distance", "Water distance", "hcsr04", "STRAIGHTEN_ROUNDED", 0),
    MetricInfo("ph", "Solution pH", "ph", "SCIENCE_ROUNDED", 2),
    MetricInfo("tds", "Nutrient strength (TDS)", "tds", "GRAIN_ROUNDED", 0),
    MetricInfo("tvoc", "TVOC", "sgp30", "AIR_ROUNDED", 0),
    MetricInfo("co2eq", "CO2 equivalent", "sgp30", "CO2_ROUNDED", 0),
)
BY_METRIC = {item.key: item for item in METRICS}

# Fallback when the controller has not answered get_sensors yet.
SENSORS = (
    SensorInfo("bme280", "BME280 air temperature, humidity, pressure",
               ("air_temperature", "air_humidity", "air_pressure"), "SGP30 loses humidity compensation"),
    SensorInfo("ds18b20", "DS18B20 water temperature", ("water_temperature",),
               "TDS loses temperature compensation"),
    SensorInfo("light", "DFR0026 light level", ("light_level",), "Automatic lamp stays OFF"),
    SensorInfo("hcsr04", "HC-SR04 water level", ("water_distance",)),
    SensorInfo("ph", "pH-4502C solution pH", ("ph",)),
    SensorInfo("tds", "SEN0244 TDS", ("tds",)),
    SensorInfo("sgp30", "SGP30 TVOC / CO2eq", ("tvoc", "co2eq")),
)
BY_SENSOR = {item.key: item for item in SENSORS}

HEALTH_LABELS = {
    "OK": "Live",
    "UNCALIBRATED": "Uncalibrated",
    "STARTING": "Starting",
    "WARMING": "Warming up",
    "STALE": "Stale",
    "FAULT": "Fault",
    "TIMEOUT": "Timeout",
    "UNPOWERED": "Unpowered",
    "DEPENDENCY_INVALID": "Dependency missing",
}


# Firmware freshness limits (alarmStaleLimitMs: BME280 1 s, others 3 s). The
# app sees a value once per 3 s telemetry frame, so it allows that period
# and some link slack before it shows a retained value as stale.
FIRMWARE_STALE_MS = {"air_temperature": 1000, "air_humidity": 1000, "air_pressure": 1000}
FIRMWARE_STALE_DEFAULT_MS = 3000
TELEMETRY_PERIOD_S = 3.0
TELEMETRY_SLACK_S = 4.0
USABLE_HEALTH = ("OK", "UNCALIBRATED")


def stale_after_s(key: str) -> float:
    """Age beyond which the app no longer shows a metric's last value as current."""
    return FIRMWARE_STALE_MS.get(key, FIRMWARE_STALE_DEFAULT_MS) / 1000 + TELEMETRY_PERIOD_S + TELEMETRY_SLACK_S


def live_health(key: str, health: str, age_s: float | None) -> str:
    """The reported health, aged locally: a usable value that has stopped arriving is STALE."""
    if health in USABLE_HEALTH and (age_s is None or age_s > stale_after_s(key)):
        return "STALE"
    return health


def metric_label(key: str) -> str:
    info = BY_METRIC.get(key)
    return info.label if info else key.replace("_", " ").capitalize()


def format_value(key: str, value: float | None) -> str:
    if value is None:
        return "—"
    info = BY_METRIC.get(key)
    decimals = info.decimals if info else 2
    return f"{value:,.{decimals}f}"


def sensor_parse(item: dict) -> SensorInfo | None:
    """Validates one get_sensors entry."""
    key = item.get("key") if isinstance(item, dict) else None
    if not isinstance(key, str) or not key:
        return None
    metrics = tuple(m for m in item.get("metrics", []) if isinstance(m, str))
    fallback = BY_SENSOR.get(key)
    label = item.get("label") if isinstance(item.get("label"), str) else (fallback.label if fallback else key)
    note = item.get("depends_note") if isinstance(item.get("depends_note"), str) else ""
    return SensorInfo(key[:16], label[:80], metrics or (fallback.metrics if fallback else ()), note[:120])

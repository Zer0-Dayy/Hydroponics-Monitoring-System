"""Contract v2 framing, requests and message validation (see FirmwareContract.md).

Length-prefixed JSON frames travel over the BLE characteristics (the only link
to the controller). Everything received is validated here before the UI or the
local store sees it.
"""

import json
import math
import struct
import uuid


SERVICE_UUID = "98a90f00-346e-4e65-9cbd-7687685bdc01"
STATE_UUID = "98a90f01-346e-4e65-9cbd-7687685bdc01"
TELEMETRY_UUID = "98a90f02-346e-4e65-9cbd-7687685bdc01"
COMMAND_UUID = "98a90f03-346e-4e65-9cbd-7687685bdc01"
RESULT_UUID = "98a90f04-346e-4e65-9cbd-7687685bdc01"

VERSION = 2
MAX_MESSAGE = 4096

COMMANDS = {
    "get_sensors", "set_sensor_enabled", "reset_latch", "set_wifi", "forget_wifi",
    "get_calibration", "cal_capture", "set_calibration", "reset_calibration", "get_diagnostics",
}
# Sensors calibrated on the device (Calibration page). Values on the wire are
# scaled integers: x = raw input x x_scale, y = reference x y_scale.
CALIBRATABLE = {
    "ph": {"points": 2, "x_scale": 10, "y_scale": 1000, "x_unit": "mV", "y_unit": "pH"},
    "tds": {"points": 1, "x_scale": 10, "y_scale": 10, "x_unit": "ppm", "y_unit": "ppm"},
    "light": {"points": 1, "x_scale": 10, "y_scale": 10, "x_unit": "mV", "y_unit": "lux"},
}
POWER_MODES = ("unknown", "normal", "transition_to_low", "low_power", "transition_to_normal", "degraded")
HEALTH = {
    "STARTING", "WARMING", "OK", "STALE", "FAULT", "TIMEOUT",
    "UNPOWERED", "UNCALIBRATED", "DEPENDENCY_INVALID",
}
METRIC_KEYS = (
    "air_temperature", "air_humidity", "air_pressure", "water_temperature", "light_level",
    "water_distance", "ph", "tds", "tvoc", "co2eq",
)


def encode(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_MESSAGE:
        raise ValueError("Message exceeds protocol limit")
    return struct.pack("<H", len(body)) + body


def request(operation: str, data: dict) -> dict:
    if operation not in COMMANDS:
        raise ValueError("Unsupported operation")
    return {"v": VERSION, "id": str(uuid.uuid4()), "op": operation, "data": data}


class Decoder:
    """Reassembles frames split or merged across packets."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, packet: bytes) -> list[dict]:
        self.buffer.extend(packet)
        messages = []
        while len(self.buffer) >= 2:
            length = struct.unpack_from("<H", self.buffer)[0]
            if not 0 < length <= MAX_MESSAGE:
                self.buffer.clear()
                raise ValueError("Invalid frame length")
            if len(self.buffer) < length + 2:
                break
            body = bytes(self.buffer[2:length + 2])
            del self.buffer[:length + 2]
            message = json.loads(body)
            if not isinstance(message, dict) or message.get("v") != VERSION:
                raise ValueError("Unsupported message version")
            messages.append(message)
        return messages


def _int(value, low=0, high=2**63) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        return None
    return value


def _text(value, limit: int, default: str = "") -> str:
    return value[:limit] if isinstance(value, str) else default


def parse_telemetry(message: dict) -> list[dict]:
    """Returns one sample per known metric; malformed entries are skipped."""
    metrics = message.get("m")
    if message.get("type") != "telemetry" or not isinstance(metrics, dict):
        raise ValueError("Not a telemetry frame")
    samples = []
    for key in METRIC_KEYS:
        entry = metrics.get(key)
        if not isinstance(entry, dict):
            continue
        health = entry.get("health")
        seq = _int(entry.get("seq"), 0, 2**32 - 1)
        if health not in HEALTH or seq is None:
            continue
        value = entry.get("value")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            value = None
        samples.append({
            "metric": key,
            "value": float(value) if value is not None else None,
            "unit": _text(entry.get("unit"), 12),
            "health": health,
            "age_ms": _int(entry.get("age_ms"), 0, 10**12),
            "seq": seq,
            "quality": _int(entry.get("q"), 0, 0xFFFF) or 0,
            "cal_rev": _int(entry.get("cal_rev"), 0, 0xFFFF) or 0,
        })
    return samples


def parse_state(message: dict) -> dict:
    """Normalises a state frame; unknown or malformed fields fall back to safe defaults."""
    if message.get("type") != "state":
        raise ValueError("Not a state frame")
    wifi = message.get("wifi") if isinstance(message.get("wifi"), dict) else {}
    safety = message.get("safety") if isinstance(message.get("safety"), dict) else {}
    enabled = message.get("enabled") if isinstance(message.get("enabled"), list) else []

    def rules(name):
        items = safety.get(name)
        return sorted({item for item in items if _int(item, 0, 15) is not None}) if isinstance(items, list) else []

    return {
        "node": _text(message.get("node"), 64, "Controller") or "Controller",
        "fw": _text(message.get("fw"), 24),
        "boot": _int(message.get("boot"), 0, 2**32 - 1) or 0,
        "uptime_ms": _int(message.get("uptime_ms")) or 0,
        "link": _text(message.get("link"), 8),
        "mode": message.get("mode") if message.get("mode") in POWER_MODES else "unknown",
        "power_epoch": _int(message.get("power_epoch"), 0, 2**32 - 1) or 0,
        "cfg_rev": _int(message.get("cfg_rev"), 0, 0xFFFF) or 0,
        "wifi": {
            "state": _text(wifi.get("state"), 16, "unconfigured") or "unconfigured",
            "ssid": _text(wifi.get("ssid"), 32),
            "ip": _text(wifi.get("ip"), 45),
            "rssi": _int(wifi.get("rssi"), -128, 0),
        },
        "safety": {
            "relay": safety.get("relay") is True,
            "lamp_reason": _text(safety.get("lamp_reason"), 16),
            "interlock": safety.get("interlock") is True,
            "latched": rules("latched"),
            "active": rules("active"),
            "stale": rules("stale"),
        },
        "sgp30_baseline": _text(message.get("sgp30_baseline"), 12),
        "enabled": [item for item in enabled if isinstance(item, str)][:16],
    }


def parse_event(message: dict) -> dict:
    if message.get("type") != "event":
        raise ValueError("Not an event frame")
    n = _int(message.get("n"), 0, 2**32 - 1)
    rule = _int(message.get("rule"), 0, 15)
    if n is None or rule is None:
        raise ValueError("Malformed event")
    return {
        "n": n,
        "rule": rule,
        "level": _text(message.get("level"), 12),
        "kind": _text(message.get("kind"), 12),
        "age_ms": _int(message.get("age_ms"), 0, 10**12) or 0,
        "gap": message.get("gap") is True,
    }


def parse_calibration(data: dict) -> dict:
    """get_calibration result -> {sensor: {overridden, rev, valid, slope, offset}} for known sensors."""
    table = data.get("calibration") if isinstance(data, dict) else None
    if not isinstance(table, dict):
        raise ValueError("Malformed calibration")
    out = {}
    for key in CALIBRATABLE:
        entry = table.get(key)
        if not isinstance(entry, dict):
            continue
        numbers = [entry.get("slope"), entry.get("offset")]
        if any(isinstance(n, bool) or not isinstance(n, (int, float)) or not math.isfinite(n) for n in numbers):
            continue
        out[key] = {
            "overridden": entry.get("overridden") is True,
            "rev": _int(entry.get("rev"), 0, 0xFFFF) or 0,
            "valid": entry.get("valid") is True,
            "slope": float(numbers[0]),
            "offset": float(numbers[1]),
        }
    return out


def parse_diagnostics(data: dict) -> dict:
    """get_diagnostics result -> hardware figures; a section that is missing or malformed becomes None."""
    if not isinstance(data, dict):
        raise ValueError("Malformed diagnostics")
    u32 = 2**32 - 1

    def section(name, keys):
        entry = data.get(name)
        if not isinstance(entry, dict):
            return None
        values = {key: _int(entry.get(key), 0, u32) for key in keys}
        return None if any(v is None for v in values.values()) else values

    cpu = data.get("cpu")
    cores = None
    if isinstance(cpu, list) and cpu:
        cores = []
        for core in cpu[:2]:
            load = _int(core.get("load"), 0, 100) if isinstance(core, dict) else None
            peak = _int(core.get("peak"), 0, 100) if isinstance(core, dict) else None
            if load is None or peak is None:
                cores = None
                break
            cores.append({"load": load, "peak": max(peak, load)})
    stacks = []
    for item in data.get("stacks") if isinstance(data.get("stacks"), list) else []:
        if not isinstance(item, dict) or not isinstance(item.get("task"), str):
            continue
        size, free_min = _int(item.get("size"), 1, u32), _int(item.get("free_min"), 0, u32)
        core = _int(item.get("core"), 0, 1)
        if size is None or free_min is None or core is None:
            continue
        stacks.append({"task": item["task"][:16], "core": core, "size": size, "free_min": min(free_min, size)})
    heap = section("heap", ("total", "free", "min", "largest"))
    if heap is not None and not heap["free"] <= heap["total"]:
        heap = None
    return {
        "uptime_ms": _int(data.get("uptime_ms")) or 0,
        "reset_reason": _text(data.get("reset_reason"), 16, "UNKNOWN") or "UNKNOWN",
        "cpu_mhz": _int(data.get("cpu_mhz"), 0, 1000) or 0,
        "cpu": cores,
        "heap": heap,
        "psram": section("psram", ("total", "free")),
        "flash": section("flash", ("app_partition", "nvs_used", "nvs_total")),
        "stacks": stacks[:16],
    }

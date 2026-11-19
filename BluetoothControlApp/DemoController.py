"""Simulated contract-v2 controller for UI work without ESP32 hardware (--demo).

It behaves like the firmware where the UI depends on it: 1 s samples sent in
3 s telemetry frames, state every 2 s, per-metric health (SGP30 warm-up, pH in
pH units but UNCALIBRATED on the nominal fit, light in mV until a lux fit),
sensor enable/disable, a latching over-temperature rule with events, Wi-Fi
provisioning, on-device calibration (capture / fit / reset), the WP9 power
modes and plausible hardware diagnostics (CPU load, heap, stacks). Demo switches move Bluetooth out of range, take Wi-Fi down or pull the
external 5 V (low-power mode).
"""

import asyncio
import random
import time

from Catalog import BY_SENSOR, SENSORS
from Links import Link, Target
from Protocol import CALIBRATABLE, VERSION

DEMO_NODE = "Hydro-ESP1 (demo)"
DEMO_ADDRESS = "DE:MO:00:00:00:01"
# Raw inputs the firmware's fits use: pH module mV, TDS nominal ppm, light pin mV.
RAW_BASES = {"ph": (2650.0, 4.0), "tds": (700.0, 6.0), "light": (900.0, 30.0)}
NOMINAL_PH = (-0.0057, 7.0 + 0.0057 * 2758.0)  # slope, offset (policy_config.h)
FIVE_VOLT = {"hcsr04", "ph", "tds"}

# metric: (base, jitter, unit)
BASES = {
    "air_temperature": (24.0, 0.3, "degC"), "air_humidity": (58.0, 1.5, "%RH"),
    "air_pressure": (101300.0, 30.0, "Pa"), "water_temperature": (21.0, 0.1, "degC"),
    "light_level": (900.0, 40.0, "mV"), "water_distance": (182.0, 2.0, "mm"),
    "ph": (6.2, 0.02, "pH"), "tds": (760.0, 8.0, "ppm"), "tvoc": (60.0, 10.0, "ppb"),
    "co2eq": (520.0, 25.0, "ppm"),
}
SENSOR_OF = {metric: sensor.key for sensor in SENSORS for metric in sensor.metrics}
# Firmware task table (include/app_config.h): name, core, stack bytes, typical free minimum.
DEMO_TASKS = (("Safety", 1, 3072, 1180), ("PowerCoord", 0, 4096, 2210), ("AcqNormal", 1, 4096, 1460),
              ("AcqLowPower", 0, 4096, 2650), ("WifiMqtt", 0, 6144, 2900), ("BleTransport", 0, 6144, 2480),
              ("Configuration", 0, 4096, 1920), ("Supervisor", 0, 3072, 1010), ("SerialLogger", 1, 2048, 760))
OVERHEAT_RULE = 0


class DemoController:
    def __init__(self):
        self.boot = random.getrandbits(32)
        self.started = time.monotonic()
        self.enabled = {sensor.key for sensor in SENSORS}
        self.cfg_rev = 1
        self.wifi_ssid = ""
        self.wifi_state = "unconfigured"
        self.wifi_outage = False
        self.ble_in_range = True
        self.external_power = True
        self.mode = "normal"
        self.power_epoch = 1
        self.heat = False
        self.raw = {key: base for key, (base, _jitter) in RAW_BASES.items()}
        self.calibration = self._default_calibration()
        self.active: set[int] = set()
        self.latched: set[int] = set()
        self.event_n = 0
        self.readings = {m: {"value": None, "health": "STARTING", "end": None, "seq": 0} for m in BASES}
        self.heap_min = 171_000
        self.cpu_peaks = [[], []]
        self.links: set["DemoLink"] = set()
        self.task: asyncio.Task | None = None

    # ------------------------------------------------------------ clock

    def now_ms(self) -> int:
        return int((time.monotonic() - self.started) * 1000)

    def ensure_running(self) -> None:
        if self.task is None or self.task.done():
            self.task = asyncio.get_running_loop().create_task(self._loop())

    async def _loop(self):
        tick = 0
        while self.links:
            self._sample()
            if tick % 2 == 0:
                self._broadcast(self.state())
            if tick % 3 == 0:
                self._broadcast(self.telemetry())
            tick += 1
            await asyncio.sleep(1)

    def _broadcast(self, message: dict) -> None:
        for link in list(self.links):
            link.deliver(message)

    # ------------------------------------------------------------ simulation

    @staticmethod
    def _default_calibration() -> dict:
        return {"ph": {"overridden": False, "rev": 0, "valid": True, "slope": NOMINAL_PH[0], "offset": NOMINAL_PH[1]},
                "tds": {"overridden": False, "rev": 0, "valid": False, "slope": 1.0, "offset": 0.0},
                "light": {"overridden": False, "rev": 0, "valid": False, "slope": 1.0, "offset": 0.0}}

    def _sample(self) -> None:
        now = self.now_ms()
        for key, (base, jitter) in RAW_BASES.items():
            self.raw[key] = base + random.uniform(-jitter, jitter)
        low_power = self.mode in ("low_power", "transition_to_low", "degraded")
        for metric, (base, jitter, unit) in BASES.items():
            r = self.readings[metric]
            sensor = SENSOR_OF[metric]
            if sensor not in self.enabled:
                continue  # not scheduled: the last reading ages
            r["unit"] = unit
            if sensor in FIVE_VOLT and (low_power or not self.external_power):
                r.update(health="UNPOWERED", value=None)
            elif metric in ("tvoc", "co2eq") and now < 15000:
                r.update(health="WARMING", value=None)
            elif metric == "ph":
                cal = self.calibration["ph"]
                r.update(value=round(cal["slope"] * self.raw["ph"] + cal["offset"], 2),
                         health="OK" if cal["overridden"] else "UNCALIBRATED")
            elif metric == "tds":
                cal = self.calibration["tds"]
                value = self.raw["tds"] * (cal["slope"] if cal["valid"] else 1.0)
                r.update(value=round(value, 1), health="OK" if cal["valid"] else "UNCALIBRATED")
            elif metric == "light_level":
                cal = self.calibration["light"]
                if cal["valid"]:
                    r.update(value=round(cal["slope"] * self.raw["light"], 0), health="OK", unit="lux")
                else:
                    r.update(value=round(self.raw["light"], 0), health="UNCALIBRATED")
            else:
                value = base + random.uniform(-jitter, jitter)
                if metric == "air_temperature" and self.heat:
                    value = 42.0 + random.uniform(0, 0.4)
                r.update(value=round(value, 2), health="OK")
            r["end"] = now
            r["seq"] += 1
        self._evaluate_overheat()

    def _evaluate_overheat(self) -> None:
        if "bme280" not in self.enabled:
            return
        value = self.readings["air_temperature"]["value"]
        if value is None:
            return
        if value >= 40.0 and OVERHEAT_RULE not in self.active:
            self.active.add(OVERHEAT_RULE)
            self.latched.add(OVERHEAT_RULE)
            self._event("trip")
        elif value <= 37.0 and OVERHEAT_RULE in self.active:
            self.active.discard(OVERHEAT_RULE)
            self._event("clear")

    def _event(self, kind: str) -> None:
        self.event_n += 1
        self._broadcast({"v": VERSION, "type": "event", "n": self.event_n, "rule": OVERHEAT_RULE,
                         "level": "CRITICAL", "kind": kind, "age_ms": 40, "gap": False})
        self._broadcast(self.state())

    # ------------------------------------------------------------ frames

    def state(self) -> dict:
        light = self.raw["light"]
        interlock = bool(self.active | self.latched)
        rail_down = not self.external_power or self.mode != "normal"
        lamp_on = light < 1000 and "light" in self.enabled and not interlock and not rail_down
        wifi = {"state": self.wifi_state}
        if self.wifi_ssid:
            wifi["ssid"] = self.wifi_ssid
        if self.wifi_state == "connected" and self.mode == "normal":
            wifi.update(ip="192.168.1.42", rssi=-58)
        return {
            "v": VERSION, "type": "state", "node": DEMO_NODE, "fw": "0.9.0-demo", "boot": self.boot,
            "uptime_ms": self.now_ms(), "link": "ble", "mode": self.mode, "power_epoch": self.power_epoch,
            "cfg_rev": self.cfg_rev, "wifi": wifi,
            "safety": {"relay": lamp_on, "lamp_reason": "interlock" if interlock else "no 5 V" if rail_down else
                       ("level" if "light" in self.enabled else "stale"),
                       "interlock": interlock, "latched": sorted(self.latched), "active": sorted(self.active),
                       "stale": [OVERHEAT_RULE] if "bme280" not in self.enabled and self.latched else []},
            "sgp30_baseline": "learning" if self.now_ms() < 60000 else "valid",
            "enabled": [s.key for s in SENSORS if s.key in self.enabled],
        }

    def telemetry(self) -> dict:
        now = self.now_ms()
        metrics = {}
        for metric, (_base, _jitter, unit) in BASES.items():
            r = self.readings[metric]
            unit = r.get("unit", unit)
            age = now - r["end"] if r["end"] is not None else None
            health, value = r["health"], r["value"]
            limit = 1000 if SENSOR_OF[metric] == "bme280" else 3000
            if health in ("OK", "UNCALIBRATED") and age is not None and age > limit:
                health, value = "STALE", None
            if SENSOR_OF[metric] not in self.enabled:
                value = None
            metrics[metric] = {"value": value if health in ("OK", "UNCALIBRATED") else None, "unit": unit,
                               "health": health, "age_ms": age, "seq": r["seq"], "q": 0,
                               "cal_rev": self._cal_rev(metric)}
        return {"v": VERSION, "type": "telemetry", "boot": self.boot, "cfg_rev": self.cfg_rev, "m": metrics}

    def _cal_rev(self, metric: str) -> int:
        sensor = {"light_level": "light"}.get(metric, metric)
        return self.calibration[sensor]["rev"] if sensor in self.calibration else 0

    # ------------------------------------------------------------ commands

    async def command(self, operation: str, data: dict) -> dict:
        await asyncio.sleep(0.2)
        if operation == "get_sensors":
            return {"sensors": [{"key": s.key, "label": s.label, "metrics": list(s.metrics),
                                 "enabled": s.key in self.enabled, "depends_note": s.depends_note}
                                for s in SENSORS]}
        if operation == "set_sensor_enabled":
            key, enabled = data.get("sensor"), data.get("enabled")
            if key not in BY_SENSOR or not isinstance(enabled, bool):
                raise RuntimeError("bad_field")
            if not enabled and key == "bme280" and (self.latched | self.active):
                raise RuntimeError("rule_latched")
            if (key in self.enabled) != enabled:
                self.enabled.add(key) if enabled else self.enabled.discard(key)
                self.cfg_rev += 1
            await asyncio.sleep(0.6)  # applied at the next frame boundary
            self._broadcast(self.state())
            return {"applied": True, "cfg_rev": self.cfg_rev}
        if operation == "reset_latch":
            rule = data.get("rule")
            if rule not in self.latched:
                raise RuntimeError("bad_field")
            if rule in self.active:
                raise RuntimeError("rule_active")
            self.latched.discard(rule)
            self._event("reset")
            return {"applied": True}
        if operation == "set_wifi":
            ssid, password = data.get("ssid", ""), data.get("password", "")
            if not ssid or len(ssid.encode()) > 32 or not 8 <= len(password) <= 63:
                raise RuntimeError("bad_field")
            self.wifi_ssid = ssid
            self.wifi_state = "connecting"
            asyncio.get_running_loop().call_later(3, self._wifi_up)
            return {"stored": True}
        if operation == "forget_wifi":
            self.wifi_ssid, self.wifi_state = "", "unconfigured"
            return {"stored": True}
        if operation == "get_calibration":
            return {"calibration": {key: dict(value) for key, value in self.calibration.items()}}
        if operation in ("cal_capture", "set_calibration", "reset_calibration"):
            return await self._calibration_command(operation, data)
        if operation == "get_diagnostics":
            return self.diagnostics()
        raise RuntimeError("unknown_op")

    def diagnostics(self) -> dict:
        """Like the firmware's Supervisor snapshot: radios on core 0, acquisition and Safety on core 1."""
        uptime = self.now_ms()
        wifi = self.wifi_state == "connected" and self.mode == "normal"
        busy = len(self.enabled) / len(SENSORS)
        loads = [round(4 + 2 * busy + (5 if wifi else 0) + random.uniform(0, 3)),
                 round(2 + 5 * busy + random.uniform(0, 2))]
        cpu = []
        for core, load in enumerate(loads):
            self.cpu_peaks[core] = (self.cpu_peaks[core] + [load])[-6:]  # 60 s at one request per 10 s
            cpu.append({"load": load, "peak": max(self.cpu_peaks[core]) + (9 if uptime < 20000 else 0)})
        free = 180_000 - (14_000 if wifi else 0) + random.randint(-1500, 1500)
        self.heap_min = min(self.heap_min, free - 2000)
        return {"uptime_ms": uptime, "reset_reason": "POWERON", "cpu_mhz": 240, "cpu": cpu,
                "heap": {"total": 352_000, "free": free, "min": self.heap_min, "largest": 110_592},
                "psram": {"total": 8_388_608, "free": 8_351_000},
                "flash": {"app_partition": 3_342_336, "nvs_used": 47, "nvs_total": 504},
                "stacks": [{"task": name, "core": core, "size": size, "free_min": free_min}
                           for name, core, size, free_min in DEMO_TASKS]}

    async def _calibration_command(self, operation: str, data: dict) -> dict:
        sensor = data.get("sensor")
        if sensor not in CALIBRATABLE:
            raise RuntimeError("bad_field")
        spec = CALIBRATABLE[sensor]
        if operation == "cal_capture":
            powered = sensor not in FIVE_VOLT or (self.external_power and self.mode == "normal")
            if sensor not in self.enabled or not powered:
                raise RuntimeError("stale")
            if sensor == "tds" and "ds18b20" not in self.enabled:
                raise RuntimeError("stale")
            return {"sensor": sensor, "x": round(self.raw[sensor] * spec["x_scale"]), "age_ms": 400}
        if operation == "reset_calibration":
            self.calibration[sensor] = self._default_calibration()[sensor]
        else:
            points = []
            for i in range(1, spec["points"] + 1):
                x, y = data.get(f"x{i}"), data.get(f"y{i}")
                if not isinstance(x, int) or not isinstance(y, int) or x <= 0 or y < 0:
                    raise RuntimeError("bad_field")
                points.append((x / spec["x_scale"], y / spec["y_scale"]))
            if sensor == "ph":
                (x1, y1), (x2, y2) = points
                if abs(y2 - y1) < 1 or x1 == x2:
                    raise RuntimeError("bad_fit")
                slope = (y2 - y1) / (x2 - x1)
                if not 0.002 <= abs(slope) <= 0.02:
                    raise RuntimeError("bad_fit")
                offset = y1 - slope * x1
            else:
                (x1, y1), = points
                slope, offset = y1 / x1, 0.0
                low, high = (0.5, 2.0) if sensor == "tds" else (0.01, 100.0)
                if not low <= slope <= high:
                    raise RuntimeError("bad_fit")
            self.cfg_rev += 1
            self.calibration[sensor] = {"overridden": True, "rev": self.cfg_rev, "valid": True,
                                        "slope": slope, "offset": offset}
        if operation == "reset_calibration":
            self.cfg_rev += 1
        await asyncio.sleep(0.6)  # stored, then applied at the next frame boundary
        self._broadcast(self.state())
        return {"applied": True, "cfg_rev": self.cfg_rev}

    def _wifi_up(self) -> None:
        if self.wifi_ssid and self.mode == "normal":
            self.wifi_state = "failed" if self.wifi_outage else "connected"
            self._broadcast(self.state())

    # ------------------------------------------------------------ demo switches

    def set_wifi_outage(self, outage: bool) -> None:
        self.wifi_outage = outage
        if outage:
            if self.wifi_ssid:
                self.wifi_state = "connecting"
        elif self.wifi_ssid:
            asyncio.get_running_loop().call_later(2, self._wifi_up)
        self._broadcast(self.state())

    def set_external_power(self, present: bool) -> None:
        """Pulls or restores the external 5 V: walks the WP9 transitions like the firmware."""
        if present == self.external_power:
            return
        self.external_power = present
        self.power_epoch += 1
        loop = asyncio.get_running_loop()
        if not present:
            self._set_mode("transition_to_low")
            loop.call_later(0.3, self._enter_low)
        else:
            loop.call_later(0.5, self._start_return)

    def _set_mode(self, mode: str) -> None:
        self.mode = mode
        self._broadcast(self.state())

    def _enter_low(self) -> None:
        if not self.external_power:
            if self.wifi_ssid:
                self.wifi_state = "connecting"  # radio off; rejoins after the return
            self._set_mode("low_power")

    def _start_return(self) -> None:
        if self.external_power and self.mode == "low_power":
            self._set_mode("transition_to_normal")
            asyncio.get_running_loop().call_later(0.4, self._finish_return)

    def _finish_return(self) -> None:
        if self.external_power:
            self._set_mode("normal")
            if self.wifi_ssid:
                asyncio.get_running_loop().call_later(2, self._wifi_up)

    def set_ble_in_range(self, in_range: bool) -> None:
        self.ble_in_range = in_range
        if not in_range:
            for link in list(self.links):
                link.drop("Bluetooth connection lost")

    def set_heat(self, heat: bool) -> None:
        self.heat = heat


class DemoLink(Link):
    """The simulated Bluetooth link to the DemoController."""

    kind = "ble"
    can_command = True

    def __init__(self, controller: DemoController, on_message, on_disconnect):
        super().__init__(on_message, on_disconnect)
        self.controller = controller

    async def connect(self, target: Target) -> None:
        await asyncio.sleep(0.4)
        c = self.controller
        if not c.ble_in_range:
            raise ConnectionError("The controller is out of Bluetooth range")
        self.target = target
        self.connected = True
        c.links.add(self)
        c.ensure_running()
        self.deliver(c.state())

    def deliver(self, message: dict) -> None:
        if self.connected:
            self.on_message(self, message)

    def drop(self, reason: str) -> None:
        if self.connected:
            self.connected = False
            self.controller.links.discard(self)
            self.on_disconnect(self, reason)

    async def disconnect(self) -> None:
        self.connected = False
        self.controller.links.discard(self)

    async def send(self, operation: str, data: dict) -> dict:
        if not self.connected:
            raise ConnectionError("Connect to the controller over Bluetooth first")
        return await self.controller.command(operation, data)


def demo_ble_targets(controller: DemoController) -> list[Target]:
    return [Target("ble", DEMO_NODE, DEMO_ADDRESS, rssi=-52)] if controller.ble_in_range else []

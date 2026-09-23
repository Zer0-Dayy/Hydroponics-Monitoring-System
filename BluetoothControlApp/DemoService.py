"""Local controller simulator for UI review before ESP32-S3 BLE firmware exists."""

import asyncio
import random
from dataclasses import dataclass

from Catalog import BY_KEY
from Protocol import VERSION


@dataclass
class Controller:
    label: str
    address: str
    device: object = None


class DemoService:
    def __init__(self, on_state, on_telemetry, on_disconnect):
        self.on_state = on_state
        self.on_telemetry = on_telemetry
        self.on_disconnect = on_disconnect
        self.connected = False
        self.low_power = False
        self.devices = [
            {"id": "demo-level", "type": "level", "label": "Reservoir level", "online": True},
            {"id": "demo-ph", "type": "ph", "label": "Solution pH", "online": True},
            {"id": "demo-tds", "type": "tds", "label": "Nutrient strength", "online": True},
            {"id": "demo-climate", "type": "climate", "label": "Air temperature & humidity", "online": True},
        ]
        self.task: asyncio.Task | None = None

    async def scan(self):
        await asyncio.sleep(0.3)
        return [Controller("Demo ESP32-S3", "DEMO-CONTROLLER")]

    async def connect(self, _controller):
        self.connected = True
        self._state()
        self.task = asyncio.create_task(self._loop())

    async def disconnect(self):
        self.connected = False
        if self.task:
            self.task.cancel()
            try:
                await self.task
            except asyncio.CancelledError:
                pass
            self.task = None

    def set_low_power(self, enabled: bool):
        self.low_power = enabled
        self._state()

    def _state(self):
        if self.connected:
            self.on_state({"v": VERSION, "node": "ESP1 Demo", "mode": "low_power" if self.low_power else "normal",
                           "wifi": "disabled" if self.low_power else "connected", "battery_percent": 74,
                           "active_types": ["level", "ph", "tds"] if self.low_power else list(BY_KEY)})

    async def _loop(self):
        while self.connected:
            self._state()
            for device in self.devices:
                kind = BY_KEY[device["type"]]
                if kind.kind != "sensor" or (self.low_power and not kind.essential):
                    continue
                base = {"level": 24.0, "ph": 6.1, "tds": 740.0, "climate": 25.0, "light": 780.0, "gas": 0.0}[kind.key]
                value = round(base + random.uniform(-0.5, 0.5), 2)
                self.on_telemetry({"v": VERSION, "node": "ESP1 Demo", "device": device["id"],
                                   "metric": kind.label, "value": value, "unit": kind.unit})
            await asyncio.sleep(3)

    async def send(self, operation, data):
        if not self.connected:
            raise ConnectionError("Connect to a controller first")
        await asyncio.sleep(0.25)
        if operation == "get_devices":
            return {"devices": list(self.devices)}
        if operation == "set_wifi":
            if self.low_power:
                raise RuntimeError("WiFi is disabled in low-power mode")
            if not data.get("ssid"):
                raise ValueError("Enter a WiFi network name")
            return {"accepted": True}
        if operation == "add_device":
            kind = BY_KEY[data["type"]]
            if self.low_power and not kind.essential:
                raise RuntimeError("This sensor is unavailable in low-power mode")
            if any(item["type"] == kind.key for item in self.devices):
                raise RuntimeError("This device is already configured")
            item = {"id": f"demo-{kind.key}", "type": kind.key, "label": kind.label, "online": True}
            self.devices.append(item)
            return {"device": item}
        if operation == "delete_device":
            before = len(self.devices)
            self.devices = [item for item in self.devices if item["id"] != data["id"]]
            if len(self.devices) == before:
                raise RuntimeError("Device was not found")
            return {"deleted": True}
        raise ValueError("Unsupported operation")

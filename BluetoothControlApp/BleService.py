"""Bleak transport for the Hydroponics GATT service."""

import asyncio
from dataclasses import dataclass
from typing import Callable

from bleak import BleakClient, BleakScanner

from Protocol import (
    AUTH_UUID, COMMAND_UUID, RESULT_UUID, SERVICE_UUID, STATE_UUID, TELEMETRY_UUID,
    Decoder, encode, request,
)


@dataclass
class Controller:
    label: str
    address: str
    device: object


class BleService:
    def __init__(self, on_state: Callable, on_telemetry: Callable, on_disconnect: Callable):
        self.on_state = on_state
        self.on_telemetry = on_telemetry
        self.on_disconnect = on_disconnect
        self.client: BleakClient | None = None
        self.decoders = {uuid: Decoder() for uuid in (STATE_UUID, TELEMETRY_UUID, RESULT_UUID)}
        self.pending: dict[str, asyncio.Future] = {}
        self.write_lock = asyncio.Lock()
        self.connected = False
        self.telemetry_enabled = False

    async def scan(self) -> list[Controller]:
        found = await BleakScanner.discover(timeout=6.0, return_adv=True)
        controllers = []
        for device, advertisement in found.values():
            if SERVICE_UUID.lower() in [item.lower() for item in advertisement.service_uuids]:
                controllers.append(Controller(device.name or "Hydroponics controller", device.address, device))
        return sorted(controllers, key=lambda item: item.label.lower())

    async def connect(self, controller: Controller) -> None:
        await self.disconnect()
        client = BleakClient(controller.device, pair=True, timeout=60, disconnected_callback=self._disconnected)
        try:
            await client.connect()
            for decoder in self.decoders.values():
                decoder.buffer.clear()
            if client.services.get_characteristic(AUTH_UUID) is None:
                raise RuntimeError("Controller lacks the protected authentication characteristic")
            # ESP firmware must require encrypted, MITM-authenticated access to this read.
            if bytes(await client.read_gatt_char(AUTH_UUID)) != b"HYDRO_SECURE_V1":
                raise RuntimeError("Controller authentication check failed")
            for uuid in self.decoders:
                if client.services.get_characteristic(uuid) is None:
                    raise RuntimeError(f"Controller is missing required GATT characteristic {uuid}")
            if client.services.get_characteristic(COMMAND_UUID) is None:
                raise RuntimeError("Controller is missing the command characteristic")
            for uuid in (STATE_UUID, RESULT_UUID):
                await client.start_notify(uuid, lambda _char, data, key=uuid: self._notification(key, data))
            self.client = client
            self.connected = True
            for message in self.decoders[STATE_UUID].feed(await client.read_gatt_char(STATE_UUID)):
                self.on_state(message)
        except Exception:
            self.connected = False
            self.client = None
            await client.disconnect()
            raise

    async def set_telemetry_enabled(self, enabled: bool) -> None:
        if not self.client or not self.connected or enabled == self.telemetry_enabled:
            return
        if enabled:
            self.decoders[TELEMETRY_UUID].buffer.clear()
            await self.client.start_notify(
                TELEMETRY_UUID,
                lambda _char, data: self._notification(TELEMETRY_UUID, data),
            )
        else:
            await self.client.stop_notify(TELEMETRY_UUID)
            self.decoders[TELEMETRY_UUID].buffer.clear()
        self.telemetry_enabled = enabled

    async def disconnect(self) -> None:
        self.connected = False
        self.telemetry_enabled = False
        client, self.client = self.client, None
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ConnectionError("BLE connection closed"))
        self.pending.clear()
        if client and client.is_connected:
            await client.disconnect()

    def _disconnected(self, _client) -> None:
        if not self.connected:
            return
        self.connected = False
        self.telemetry_enabled = False
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ConnectionError("BLE connection lost"))
        self.pending.clear()
        self.on_disconnect()

    def _notification(self, characteristic: str, data: bytearray) -> None:
        try:
            messages = self.decoders[characteristic].feed(bytes(data))
            for message in messages:
                if characteristic == STATE_UUID:
                    self.on_state(message)
                elif characteristic == TELEMETRY_UUID:
                    if self.telemetry_enabled:
                        self.on_telemetry(message)
                else:
                    future = self.pending.pop(message.get("id", ""), None)
                    if future and not future.done():
                        future.set_result(message)
        except (ValueError, UnicodeError):
            # A malformed notification must not crash the UI or be treated as a valid state.
            self._disconnected(None)
            asyncio.create_task(self.disconnect())

    async def send(self, operation: str, data: dict) -> dict:
        if not self.client or not self.connected:
            raise ConnectionError("Connect to a controller first")
        message = request(operation, data)
        future = asyncio.get_running_loop().create_future()
        self.pending[message["id"]] = future
        try:
            async with self.write_lock:
                # Conservative 20-byte chunks work with the default ATT MTU. The firmware
                # reassembles the length-prefixed stream before handling the request.
                payload = encode(message)
                for offset in range(0, len(payload), 20):
                    await self.client.write_gatt_char(COMMAND_UUID, payload[offset:offset + 20], response=True)
            result = await asyncio.wait_for(future, timeout=15)
            if result.get("status") != "ok":
                raise RuntimeError(result.get("error", "Controller rejected the request"))
            return result.get("data", {})
        finally:
            self.pending.pop(message["id"], None)

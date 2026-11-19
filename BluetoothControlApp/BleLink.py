"""Bleak transport for the Hydroponics GATT service (contract v2).

The link is open (firmware 0.11.0 and later): no passkey, no pairing, the
same code on every platform. A pairing left over from older firmware is
removed first (OsBond.py), so the OS never tries to encrypt with a dead key.
State, Telemetry and Result notifications stay subscribed for the whole
connection, so readings are recorded whatever page is open.
"""

import asyncio
import logging
import sys

from bleak import BleakClient, BleakScanner

import OsBond
from Links import Link, Target
from Protocol import (
    COMMAND_UUID, RESULT_UUID, SERVICE_UUID, STATE_UUID, TELEMETRY_UUID, Decoder, encode, request,
)

log = logging.getLogger("session.ble")

NOTIFY_UUIDS = (STATE_UUID, TELEMETRY_UUID, RESULT_UUID)


async def scan_ble(timeout: float = 6.0) -> list[Target]:
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    targets = []
    for device, advertisement in found.values():
        if SERVICE_UUID.lower() in [item.lower() for item in advertisement.service_uuids]:
            targets.append(Target("ble", device.name or advertisement.local_name or "Hydroponics controller",
                                  device.address, rssi=advertisement.rssi, device=device))
    return sorted(targets, key=lambda item: item.label.lower())


class BleLink(Link):
    kind = "ble"
    can_command = True

    def __init__(self, on_message, on_disconnect):
        super().__init__(on_message, on_disconnect)
        self.client: BleakClient | None = None
        self.listening: BleakClient | None = None  # the only client whose notifications are decoded
        self.decoders = {uuid: Decoder() for uuid in NOTIFY_UUIDS}
        self.pending: dict[str, asyncio.Future] = {}
        self.write_lock = asyncio.Lock()

    async def connect(self, target: Target) -> None:
        await self.disconnect()
        device = target.device or target.address
        if await OsBond.remove_stale_bond(device) and sys.platform.startswith("linux"):
            # BlueZ dropped the device object with its pairing: find it again.
            device = await BleakScanner.find_device_by_address(target.address, timeout=8.0)
            if device is None:
                raise ConnectionError("The controller is no longer in range. Scan again with the ESP nearby.")
        # Uncached services: Windows may hold the GATT table of an older firmware
        # at this address (it had other characteristics and permissions).
        client = BleakClient(device, pair=False, timeout=30, winrt={"use_cached_services": False},
                             disconnected_callback=lambda c: self._disconnected(c))
        try:
            await client.connect()
            self._clear_decoders()
            self.listening = client
            for uuid in (COMMAND_UUID, *NOTIFY_UUIDS):
                if client.services.get_characteristic(uuid) is None:
                    raise RuntimeError(f"Controller is missing GATT characteristic {uuid}")
            for uuid in NOTIFY_UUIDS:
                await client.start_notify(
                    uuid, lambda _char, data, key=uuid, owner=client: self._notification(owner, key, data))
            self.client = client
            self.target = target
            self.connected = True
            self._deliver(STATE_UUID, await client.read_gatt_char(STATE_UUID))
        except Exception as error:
            self.connected = False
            self.client = None
            self.listening = None
            try:
                await client.disconnect()
            except Exception:
                pass
            log.info("connect to %s failed: %s: %s", target.address, type(error).__name__, error)
            text = str(error).lower()
            if any(word in text for word in ("authenticat", "encrypt", "insufficient")):
                raise ConnectionError(
                    "The controller asks for a secure link: it runs firmware older than 0.11.0. "
                    "Flash the current firmware, then connect again."
                ) from error
            raise

    async def disconnect(self) -> None:
        self.connected = False
        client, self.client = self.client, None
        self.listening = None
        self._clear_decoders()
        self._fail_pending("Bluetooth connection closed")
        await self._close(client)

    @staticmethod
    async def _close(client: BleakClient | None) -> None:
        """Always close, even after a link loss: on Windows the GATT session keeps
        "maintaining" the connection until it is closed, so the controller
        stays held by a dead client and every new connection fails."""
        if client is None:
            return
        try:
            await client.disconnect()
        except Exception:
            pass

    def _clear_decoders(self) -> None:
        for decoder in self.decoders.values():
            decoder.buffer.clear()

    def _fail_pending(self, reason: str) -> None:
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ConnectionError(reason))
        self.pending.clear()

    def _disconnected(self, client) -> None:
        if client is not None and client is not self.client:
            return  # a late callback from an earlier client
        if not self.connected:
            return
        self.connected = False
        dead, self.client = self.client, None
        self.listening = None
        self._clear_decoders()  # a frame cut by the link loss must not prefix the next one
        self._fail_pending("Bluetooth connection lost")
        if dead is not None:
            asyncio.get_running_loop().create_task(self._close(dead))
        self.on_disconnect(self, "Bluetooth connection lost")

    def _deliver(self, characteristic: str, data: bytes) -> None:
        for message in self.decoders[characteristic].feed(bytes(data)):
            if message.get("type") == "result":
                future = self.pending.pop(str(message.get("id", "")), None)
                if future and not future.done():
                    future.set_result(message)
            else:
                self.on_message(self, message)

    def _notification(self, owner: BleakClient, characteristic: str, data: bytearray) -> None:
        if owner is not self.listening:
            return  # an old session Windows kept alive: never mixed into the current stream
        try:
            self._deliver(characteristic, data)
        except (ValueError, UnicodeError):
            # A malformed frame must not crash the UI or be taken as valid data.
            self._disconnected(None)
            asyncio.get_running_loop().create_task(self.disconnect())

    def _chunk_size(self) -> int:
        mtu = getattr(self.client, "mtu_size", 23) or 23
        return max(20, min(mtu - 3, 244))

    async def send(self, operation: str, data: dict) -> dict:
        if not self.client or not self.connected:
            raise ConnectionError("Connect to the controller over Bluetooth first")
        message = request(operation, data)
        future = asyncio.get_running_loop().create_future()
        self.pending[message["id"]] = future
        try:
            async with self.write_lock:
                payload = encode(message)
                size = self._chunk_size()
                for offset in range(0, len(payload), size):
                    await self.client.write_gatt_char(COMMAND_UUID, payload[offset:offset + size], response=True)
            result = await asyncio.wait_for(future, timeout=15)
            status = result.get("status")
            if status == "busy":
                raise RuntimeError("The controller is busy. Try again in a moment.")
            if status != "ok":
                raise RuntimeError(str(result.get("error", "rejected")))
            return result.get("data", {}) if isinstance(result.get("data"), dict) else {}
        finally:
            self.pending.pop(message["id"], None)

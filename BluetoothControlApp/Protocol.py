"""Versioned BLE frame contract shared with the ESP32-S3 demo."""

import json
import struct
import uuid


VERSION = 1
MAX_MESSAGE = 4096
SERVICE_UUID = "98a90f00-346e-4e65-9cbd-7687685bdc01"
STATE_UUID = "98a90f01-346e-4e65-9cbd-7687685bdc01"
TELEMETRY_UUID = "98a90f02-346e-4e65-9cbd-7687685bdc01"
COMMAND_UUID = "98a90f03-346e-4e65-9cbd-7687685bdc01"
RESULT_UUID = "98a90f04-346e-4e65-9cbd-7687685bdc01"
AUTH_UUID = "98a90f05-346e-4e65-9cbd-7687685bdc01"


def encode(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_MESSAGE:
        raise ValueError("Message exceeds BLE protocol limit")
    return struct.pack("<H", len(body)) + body


def request(operation: str, data: dict) -> dict:
    if operation not in {"get_devices", "set_wifi", "add_device", "delete_device"}:
        raise ValueError("Unsupported operation")
    return {"v": VERSION, "id": str(uuid.uuid4()), "op": operation, "data": data}


class Decoder:
    """Reassembles a length-prefixed JSON stream from GATT notifications."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, packet: bytes) -> list[dict]:
        self.buffer.extend(packet)
        messages = []
        while len(self.buffer) >= 2:
            length = struct.unpack_from("<H", self.buffer)[0]
            if not 0 < length <= MAX_MESSAGE:
                self.buffer.clear()
                raise ValueError("Invalid BLE frame length")
            if len(self.buffer) < length + 2:
                break
            body = bytes(self.buffer[2:length + 2])
            del self.buffer[:length + 2]
            message = json.loads(body)
            if not isinstance(message, dict) or message.get("v") != VERSION:
                raise ValueError("Unsupported BLE message version")
            messages.append(message)
        return messages

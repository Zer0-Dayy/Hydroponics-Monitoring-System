"""Common shape of the controller links: Bluetooth (BleLink) and the demo.

A link delivers validated contract-v2 frames to `on_message(link, message)`
and reports a lost connection through `on_disconnect(link, reason)`.
"""

from dataclasses import dataclass, field
from typing import Callable


@dataclass
class Target:
    """Something found by discovery that a link can connect to."""
    kind: str                 # "ble"
    label: str
    address: str              # BLE address
    rssi: int | None = None
    device: object = field(default=None, repr=False)


class Link:
    kind = "?"
    can_command = False

    def __init__(self, on_message: Callable, on_disconnect: Callable):
        self.on_message = on_message
        self.on_disconnect = on_disconnect
        self.connected = False
        self.target: Target | None = None

    async def connect(self, target: Target) -> None:
        raise NotImplementedError

    async def disconnect(self) -> None:
        raise NotImplementedError

    async def send(self, operation: str, data: dict) -> dict:
        raise RuntimeError("Connect over Bluetooth to change settings")

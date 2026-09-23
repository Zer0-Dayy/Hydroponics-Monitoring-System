"""Pair an ESP through a temporary BlueZ passkey agent on Ubuntu."""

import asyncio

from dbus_fast import BusType, DBusError, Message, MessageType, Variant
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method


AGENT_PATH = "/org/hydroponics/PairingAgent"
AGENT_MANAGER = "/org/bluez"
BLUEZ = "org.bluez"


class PasskeyAgent(ServiceInterface):
    def __init__(self, device_path: str, passkey: str):
        super().__init__("org.bluez.Agent1")
        self.device_path = device_path
        self.passkey = passkey

    def _only_selected_device(self, device: str) -> None:
        if device != self.device_path:
            raise DBusError("org.bluez.Error.Rejected", "Pairing was not requested for this controller")

    @method()
    def Release(self):
        pass

    @method()
    def RequestPasskey(self, device: "o") -> "u":
        self._only_selected_device(device)
        return int(self.passkey)

    @method()
    def RequestPinCode(self, device: "o") -> "s":
        self._only_selected_device(device)
        raise DBusError("org.bluez.Error.Rejected", "Legacy PIN pairing is not supported")

    @method()
    def DisplayPinCode(self, device: "o", pincode: "s"):
        raise DBusError("org.bluez.Error.Rejected", "Unexpected PIN display request")

    @method()
    def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):
        raise DBusError("org.bluez.Error.Rejected", "Unexpected passkey display request")

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u"):
        raise DBusError("org.bluez.Error.Rejected", "Numeric comparison is not this controller's pairing mode")

    @method()
    def RequestAuthorization(self, device: "o"):
        raise DBusError("org.bluez.Error.Rejected", "Unexpected pairing authorization request")

    @method()
    def AuthorizeService(self, device: "o", uuid: "s"):
        self._only_selected_device(device)

    @method()
    def Cancel(self):
        pass


async def _call(bus: MessageBus, message: Message):
    reply = await bus.call(message)
    if reply.message_type == MessageType.ERROR:
        detail = reply.body[0] if reply.body else reply.error_name
        raise DBusError(reply.error_name or "org.bluez.Error.Failed", str(detail))
    return reply


async def _is_paired(bus: MessageBus, device_path: str) -> bool:
    reply = await _call(bus, Message(
        destination=BLUEZ, path=device_path, interface="org.freedesktop.DBus.Properties",
        member="Get", signature="ss", body=["org.bluez.Device1", "Paired"],
    ))
    return bool(reply.body[0].value)


async def pair_controller(device: object, passkey: str) -> None:
    """Pair on the agent's D-Bus connection so BlueZ uses our keyboard agent."""
    device_path = getattr(device, "details", {}).get("path")
    if not device_path:
        raise RuntimeError("BlueZ did not provide a path for this controller; scan again")

    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    registered = False
    try:
        if await _is_paired(bus, device_path):
            return
        if len(passkey) != 6 or not passkey.isascii() or not passkey.isdigit():
            raise ValueError("Enter the six-digit BLE passkey shown in the ESP serial monitor")

        agent = PasskeyAgent(device_path, passkey)
        bus.export(AGENT_PATH, agent)
        await _call(bus, Message(
            destination=BLUEZ, path=AGENT_MANAGER, interface="org.bluez.AgentManager1",
            member="RegisterAgent", signature="os", body=[AGENT_PATH, "KeyboardOnly"],
        ))
        registered = True
        try:
            await asyncio.wait_for(_call(bus, Message(
                destination=BLUEZ, path=device_path, interface="org.bluez.Device1",
                member="Pair",
            )), timeout=60)
        except DBusError as error:
            if error.type != "org.bluez.Error.AlreadyExists" or not await _is_paired(bus, device_path):
                raise

        await _call(bus, Message(
            destination=BLUEZ, path=device_path, interface="org.freedesktop.DBus.Properties",
            member="Set", signature="ssv",
            body=["org.bluez.Device1", "Trusted", Variant("b", True)],
        ))
    finally:
        if registered:
            try:
                await _call(bus, Message(
                    destination=BLUEZ, path=AGENT_MANAGER, interface="org.bluez.AgentManager1",
                    member="UnregisterAgent", signature="o", body=[AGENT_PATH],
                ))
            except Exception:
                pass
        bus.disconnect()
        await bus.wait_for_disconnect()

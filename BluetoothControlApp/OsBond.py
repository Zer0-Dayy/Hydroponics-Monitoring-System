"""Removes a Bluetooth pairing the operating system still holds for the controller.

Since firmware 0.11.0 the controller uses an open link: no passkey, no
pairing, no bond. A computer that paired with older firmware still keeps that
bond. On every connection it would then try to encrypt with a key the
controller no longer has, and the link fails with "key missing". Before this
module, the only fix was to remove the device by hand in Windows Settings or
bluetoothctl.

`remove_stale_bond()` runs before every connection. If the OS holds a pairing
for this address, it removes it, on Windows through WinRT and on Linux through
BlueZ over D-Bus. On other platforms it does nothing.
"""

import logging
import sys

log = logging.getLogger("session.ble")


async def remove_stale_bond(device) -> bool:
    """True if a pairing was found and removed. Never raises: a failure is logged
    and the connection is tried anyway."""
    try:
        if sys.platform == "win32":
            return await _windows_unpair(device)
        if sys.platform.startswith("linux"):
            return await _bluez_remove(device)
    except Exception as error:
        log.warning("could not check the OS pairing: %s: %s", type(error).__name__, error)
    return False


# ---------------------------------------------------------------- Windows (WinRT)

def _address(device) -> int:
    text = getattr(device, "address", device)
    return int(str(text).replace(":", "").replace("-", ""), 16)


async def _windows_unpair(device) -> bool:
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    from winrt.windows.devices.enumeration import DeviceInformation, DeviceUnpairingResultStatus

    le = await BluetoothLEDevice.from_bluetooth_address_async(_address(device))
    if le is None:
        return False
    try:
        # A fresh object: the one cached on the device is not updated after (un)pairing.
        info = await DeviceInformation.create_from_id_async(le.device_information.id)
    finally:
        le.close()
    if not info.pairing.is_paired:
        return False
    result = await info.pairing.unpair_async()
    if result.status not in (DeviceUnpairingResultStatus.UNPAIRED, DeviceUnpairingResultStatus.ALREADY_UNPAIRED):
        raise RuntimeError(f"Windows could not remove the old pairing ({result.status.name})")
    log.info("removed the old Windows pairing for %s", getattr(device, "address", device))
    return True


# ---------------------------------------------------------------- Linux (BlueZ)

async def _bluez_remove(device) -> bool:
    from dbus_fast import BusType, Message, MessageType
    from dbus_fast.aio import MessageBus

    device_path = getattr(device, "details", {}).get("path") if not isinstance(device, str) else None
    if not device_path:
        return False
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    try:
        reply = await bus.call(Message(
            destination="org.bluez", path=device_path, interface="org.freedesktop.DBus.Properties",
            member="Get", signature="ss", body=["org.bluez.Device1", "Paired"],
        ))
        if reply.message_type == MessageType.ERROR or not bool(reply.body[0].value):
            return False
        reply = await bus.call(Message(
            destination="org.bluez", path=device_path.rsplit("/", 1)[0], interface="org.bluez.Adapter1",
            member="RemoveDevice", signature="o", body=[device_path],
        ))
        if reply.message_type == MessageType.ERROR:
            raise RuntimeError(f"BlueZ could not remove the old pairing ({reply.error_name})")
        log.info("removed the old BlueZ pairing for %s", getattr(device, "address", device))
        return True
    finally:
        bus.disconnect()
        await bus.wait_for_disconnect()

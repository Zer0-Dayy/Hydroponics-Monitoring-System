"""Lists every Bluetooth LE device in range, for when the app's scan finds no controller.

Same bleak call as the app's scan (BleLink.scan_ble), without its service filter,
so it tells apart a PC that cannot scan at all (an error is printed), a PC that
scans but does not see the controller, and a controller that does advertise
(marked "<-- controller").

Run from the app folder:  python tools/ble_scan.py
"""

import asyncio
import sys
from pathlib import Path

from bleak import BleakScanner

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from Protocol import SERVICE_UUID  # noqa: E402

TIMEOUT_S = 10.0


async def main() -> None:
    print(f"Scanning for {TIMEOUT_S:.0f} s...")
    found = await BleakScanner.discover(timeout=TIMEOUT_S, return_adv=True)
    print(f"{len(found)} devices")
    for device, advertisement in sorted(found.values(), key=lambda item: -item[1].rssi):
        uuids = [item.lower() for item in advertisement.service_uuids]
        mark = "  <-- controller" if SERVICE_UUID.lower() in uuids else ""
        print(f"{advertisement.rssi:5} dBm  {device.address}  {device.name or advertisement.local_name or '-'}"
              f"  {uuids}{mark}")


if __name__ == "__main__":
    asyncio.run(main())

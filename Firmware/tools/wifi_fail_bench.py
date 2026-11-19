"""Wi-Fi failure-path bench (Stage 8.4): store credentials for a network that does
not exist, watch the State report 'connecting' / 'failed' with growing retries,
then forget them. Uses the app's BleLink; no pairing needed, the app closed.

From Firmware/ on Linux:
  APP=../BluetoothControlApp
  "$APP/.venv/bin/python" tools/wifi_fail_bench.py "$APP" 75
"""
import asyncio
import sys
import time

sys.path.insert(0, sys.argv[1])
from BleLink import BleLink, scan_ble  # noqa: E402
from Protocol import parse_state  # noqa: E402

states: list = []


def on_message(_link, message):
    if message.get("type") == "state":
        wifi = parse_state(message)["wifi"]["state"]
        if not states or states[-1][1] != wifi:
            states.append((round(time.monotonic() - t0, 1), wifi))


async def main(seconds: float):
    targets = await scan_ble(6.0)
    if not targets:
        raise SystemExit("controller not found")
    link = BleLink(on_message, lambda _l, reason: print("disconnected:", reason))
    await link.connect(targets[0])
    print("set_wifi:", await link.send("set_wifi", {"ssid": "NoSuchNet-Bench", "password": "not-a-real-pass"}))
    await asyncio.sleep(seconds)
    print("forget_wifi:", await link.send("forget_wifi", {}))
    await asyncio.sleep(4)
    print("State wifi.state changes (s since connect, state):", states)
    await link.disconnect()


t0 = time.monotonic()
asyncio.run(main(float(sys.argv[2])))

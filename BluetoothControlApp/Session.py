"""Controller session: the Bluetooth link, reconnects, recording and commands.

No UI code here, so it can be tested with the demo controller. Rules:

- Bluetooth is the only link to the controller: it carries the data and the
  commands. Wi-Fi is a controller setting (Wi-Fi page), never a link for the
  app. The link is open (no passkey, no pairing), so any computer can
  connect, and a lost link is re-established automatically.
- Every telemetry frame is recorded, whatever page is open; the store keeps
  one row per (node, boot, metric, seq), so the 3 s re-sends of a sample
  merge.
"""

import asyncio
import logging
import time
from typing import Callable

from Catalog import SENSORS, SensorInfo, sensor_parse
from Links import Target
from Protocol import CALIBRATABLE, parse_calibration, parse_diagnostics, parse_event, parse_state, parse_telemetry
from Store import Store

STATE_STALE_S = 15
TELEMETRY_STALE_S = 10  # three 3 s frames missed
DIAGNOSTICS_EVERY_S = 10  # get_diagnostics poll, only while the Dashboard is open
DIAGNOSTICS_STALE_S = 30
BLE_RETRY_DELAYS = (2, 4, 8)
GLOBAL = "*"  # Settings row owner for app-wide settings

log = logging.getLogger("session")  # link decisions; Main.py --log FILE writes them out


class Session:
    def __init__(self, store: Store, demo: bool, notify: Callable[[str], None] | None = None):
        self.store = store
        self.demo = demo
        user_notify = notify or (lambda _message: None)

        def notify_and_log(message: str) -> None:
            log.info("notice: %s", message)
            user_notify(message)

        self.notify = notify_and_log
        self.listeners: list[Callable[[str], None]] = []
        if demo:
            from DemoController import DemoController, DemoLink
            self.demo_controller = DemoController()
            self.ble = DemoLink(self.demo_controller, self._on_message, self._on_disconnect)
        else:
            from BleLink import BleLink
            self.demo_controller = None
            self.ble = BleLink(self._on_message, self._on_disconnect)
        self.node: str | None = store.setting(GLOBAL, "last_node")
        self.state: dict | None = None
        self.state_at = 0.0
        self.telemetry_at = 0.0
        self.readings: dict[str, dict] = {}
        self.sensors: list[SensorInfo] = list(SENSORS)
        self.calibration: dict[str, dict] = {}
        self.diagnostics: dict | None = None  # hardware figures (get_diagnostics)
        self.diagnostics_at = 0.0     # last answer
        self.diagnostics_tried = 0.0  # last request, answered or not
        self.diagnostics_supported = True  # False once the firmware answers unknown_op
        self.diagnostics_busy = False
        self.ble_target: Target | None = None
        self.want_ble = False      # the user connected and did not disconnect
        self.ble_reconnecting = False
        self.recorded = 0
        self.closed = False
        self.tasks: set[asyncio.Task] = set()

    # ------------------------------------------------------------ status

    def subscribe(self, listener: Callable[[str], None]) -> None:
        self.listeners.append(listener)

    def _emit(self, kind: str) -> None:
        for listener in list(self.listeners):
            listener(kind)

    @property
    def connected(self) -> bool:
        return self.ble.connected

    @property
    def active_link(self) -> str | None:
        return "ble" if self.ble.connected else None

    @property
    def can_command(self) -> bool:
        return self.ble.connected and self.state_fresh

    @property
    def state_fresh(self) -> bool:
        return self.connected and self.state is not None and time.monotonic() - self.state_at < STATE_STALE_S

    @property
    def enabled(self) -> set[str]:
        if self.state is None:
            return {sensor.key for sensor in self.sensors}
        return set(self.state["enabled"])

    @property
    def power_mode(self) -> str:
        return (self.state or {}).get("mode", "unknown")

    @property
    def telemetry_fresh(self) -> bool:
        """State alone keeps the link looking alive: this says data is still flowing."""
        return self.connected and self.telemetry_at > 0 and time.monotonic() - self.telemetry_at < TELEMETRY_STALE_S

    @property
    def diagnostics_fresh(self) -> bool:
        return self.diagnostics is not None and time.monotonic() - self.diagnostics_at < DIAGNOSTICS_STALE_S

    @property
    def diagnostics_due(self) -> bool:
        return (self.can_command and self.diagnostics_supported and not self.diagnostics_busy
                and time.monotonic() - self.diagnostics_tried >= DIAGNOSTICS_EVERY_S)

    def reading_age_s(self, reading: dict) -> float | None:
        if reading.get("age_ms") is None:
            return None
        return reading["age_ms"] / 1000 + (time.monotonic() - reading["received"])

    def _spawn(self, coroutine) -> asyncio.Task:
        task = asyncio.get_running_loop().create_task(coroutine)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)
        return task

    # ------------------------------------------------------------ discovery

    async def scan(self) -> list[Target]:
        """Bluetooth scan for controllers."""
        if self.demo:
            from DemoController import demo_ble_targets
            await asyncio.sleep(0.6)
            return demo_ble_targets(self.demo_controller)
        from BleLink import scan_ble
        try:
            return await scan_ble()
        except Exception as error:
            self.notify(f"Bluetooth scan failed: {error}")
            return []

    # ------------------------------------------------------------ connecting

    async def connect_ble(self, target: Target) -> None:
        await self.ble.connect(target)
        self.ble_target = target
        self.want_ble = True
        self._clear_diagnostics()
        self._emit("link")
        await self._after_ble_connect()

    async def _after_ble_connect(self) -> None:
        try:
            await self.refresh_sensors()
            await self.refresh_calibration()
        except Exception as error:
            log.warning("after BLE connect: %s: %s", type(error).__name__, error)
            self.notify(f"Connected, but the controller did not answer: {error}")

    async def disconnect(self) -> None:
        self.want_ble = False
        for task in list(self.tasks):
            task.cancel()
        await self.ble.disconnect()
        self.state = None
        self.readings.clear()
        self._clear_diagnostics()
        self._emit("link")

    # ------------------------------------------------------------ reconnect

    def _start_ble_reconnect(self) -> None:
        if not self.ble_reconnecting and not self.closed:
            self._spawn(self._ble_reconnect())

    async def _ble_reconnect(self) -> None:
        self.ble_reconnecting = True
        self._emit("link")
        try:
            for delay in BLE_RETRY_DELAYS:
                await asyncio.sleep(delay)
                if self.closed or not self.want_ble or self.ble.connected or self.ble_target is None:
                    return
                try:
                    target = await self._rescan_ble(self.ble_target.address)
                    if target is None:
                        continue
                    await self.ble.connect(target)
                    self.ble_target = target
                    self._emit("link")
                    await self._after_ble_connect()
                    if not self.ble.connected:
                        # Lost again during the handshake. _on_disconnect() could not start
                        # a new reconnect while this one runs, so keep retrying here.
                        log.info("reconnect: link lost again right after connecting")
                        continue
                    self.notify("Bluetooth reconnected.")
                    return
                except Exception as error:
                    log.info("reconnect attempt failed: %s: %s", type(error).__name__, error)
                    await self.ble.disconnect()
            self.notify("Automatic reconnect stopped. Open Connection to try again.")
        finally:
            self.ble_reconnecting = False
            self._emit("link")

    async def _rescan_ble(self, address: str) -> Target | None:
        if self.demo:
            from DemoController import demo_ble_targets
            await asyncio.sleep(0.3)
            found = demo_ble_targets(self.demo_controller)
        else:
            from BleLink import scan_ble
            found = await scan_ble(timeout=4.0)
        return next((t for t in found if t.address == address), None)

    def _on_disconnect(self, _link, reason: str) -> None:
        if self.closed:
            return
        self.notify(f"{reason}. Trying to reconnect…" if self.want_ble else reason)
        if self.want_ble:
            self._start_ble_reconnect()
        self.state = None
        self._clear_diagnostics()
        self._emit("link")

    # ------------------------------------------------------------ messages

    def _on_message(self, link, message: dict) -> None:
        if self.closed:
            return
        kind = message.get("type")
        try:
            if kind == "state":
                self._state(parse_state(message))
            elif kind == "telemetry":
                self._telemetry(message, parse_telemetry(message), link.kind)
            elif kind == "event":
                self._event(parse_event(message))
        except (ValueError, KeyError, TypeError):
            return  # a malformed frame is ignored, never shown as data

    def _state(self, state: dict) -> None:
        previous = self.state
        if state["node"] != self.node:
            self.node = state["node"]
            self.store.set_setting(GLOBAL, "last_node", self.node)
            self.readings.clear()
        if previous and previous["boot"] != state["boot"]:
            self.readings.clear()  # the controller restarted: sequence numbers start again
            self._clear_diagnostics()
        if previous and previous["mode"] != state["mode"]:
            log.info("power mode: %s -> %s", previous["mode"], state["mode"])
        self.state = state
        self.state_at = time.monotonic()
        self._emit("state")

    def _telemetry(self, message: dict, samples: list[dict], link_kind: str) -> None:
        if self.node is None or self.state is None:
            return  # the node name comes with State, which always arrives first
        boot = message.get("boot") if isinstance(message.get("boot"), int) else self.state["boot"]
        cfg_rev = message.get("cfg_rev") if isinstance(message.get("cfg_rev"), int) else 0
        received = time.monotonic()
        self.telemetry_at = received
        self.recorded += self.store.record_samples(self.node, boot, cfg_rev, samples, link_kind)
        for sample in samples:
            current = self.readings.get(sample["metric"])
            if current and current["boot"] == boot and current["seq"] > sample["seq"]:
                continue  # an older frame
            self.readings[sample["metric"]] = {**sample, "boot": boot, "received": received, "link": link_kind}
        self._emit("telemetry")

    def _event(self, event: dict) -> None:
        if self.node is None or self.state is None:
            return
        if self.store.record_event(self.node, self.state["boot"], event):
            if event["gap"]:
                self.notify("Some alarm events were lost while the link was congested.")
            if event["kind"] == "trip":
                self.notify(f"Alarm: rule {event['rule']} tripped ({event['level'].lower()}).")
        self._emit("event")

    # ------------------------------------------------------------ commands

    async def command(self, operation: str, data: dict) -> dict:
        if not self.ble.connected:
            raise RuntimeError("Connect over Bluetooth to change settings")
        return await self.ble.send(operation, data)

    async def refresh_sensors(self) -> None:
        data = await self.command("get_sensors", {})
        parsed = [sensor_parse(item) for item in data.get("sensors", [])]
        sensors = [item for item in parsed if item is not None]
        if sensors:
            self.sensors = sensors
            self._emit("sensors")

    async def set_sensor_enabled(self, key: str, enabled: bool) -> None:
        await self.command("set_sensor_enabled", {"sensor": key, "enabled": enabled})

    async def reset_latch(self, rule: int) -> None:
        await self.command("reset_latch", {"rule": rule})

    async def set_wifi(self, ssid: str, password: str) -> None:
        await self.command("set_wifi", {"ssid": ssid, "password": password})

    async def forget_wifi(self) -> None:
        await self.command("forget_wifi", {})

    # ------------------------------------------------------------ calibration

    async def refresh_calibration(self) -> None:
        self.calibration = parse_calibration(await self.command("get_calibration", {}))
        self._emit("calibration")

    async def cal_capture(self, sensor: str) -> float:
        """The sensor's raw input now, in the unit its fit uses (mV, or ppm for TDS)."""
        if sensor not in CALIBRATABLE:
            raise ValueError("bad_field")
        data = await self.command("cal_capture", {"sensor": sensor})
        x = data.get("x")
        if isinstance(x, bool) or not isinstance(x, int):
            raise RuntimeError("bad_request")
        return x / CALIBRATABLE[sensor]["x_scale"]

    async def set_calibration(self, sensor: str, points: list[tuple[float, float]]) -> int:
        """Sends the captured points (x, reference); the controller fits and stores them."""
        spec = CALIBRATABLE[sensor]
        if len(points) != spec["points"]:
            raise ValueError("bad_field")
        data = {}
        for i, (x, y) in enumerate(points, start=1):
            data[f"x{i}"] = round(x * spec["x_scale"])
            data[f"y{i}"] = round(y * spec["y_scale"])
        result = await self.command("set_calibration", {"sensor": sensor, **data})
        await self.refresh_calibration()
        return int(result.get("cfg_rev", 0))

    async def reset_calibration(self, sensor: str) -> None:
        await self.command("reset_calibration", {"sensor": sensor})
        await self.refresh_calibration()

    # ------------------------------------------------------------ hardware diagnostics

    def _clear_diagnostics(self) -> None:
        self.diagnostics = None
        self.diagnostics_at = self.diagnostics_tried = 0.0
        self.diagnostics_supported = True  # another firmware may answer

    async def refresh_diagnostics(self) -> None:
        """Copies the controller's hardware snapshot; never notifies (it is a background poll)."""
        if self.diagnostics_busy:
            return
        self.diagnostics_busy = True
        self.diagnostics_tried = time.monotonic()
        try:
            data = await self.command("get_diagnostics", {})
            self.diagnostics = parse_diagnostics(data)
            self.diagnostics_at = time.monotonic()
        except RuntimeError as error:
            if str(error) == "unknown_op":
                self.diagnostics_supported = False
            else:
                log.info("get_diagnostics: %s", error)
        except Exception as error:
            log.info("get_diagnostics: %s: %s", type(error).__name__, error)
        finally:
            self.diagnostics_busy = False
        self._emit("diagnostics")

    async def close(self) -> None:
        self.closed = True
        for task in list(self.tasks):
            task.cancel()
        await self.ble.disconnect()

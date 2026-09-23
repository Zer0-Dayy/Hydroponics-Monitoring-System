"""Hydroponics controller desktop app. Run with --demo for a local simulator."""

import argparse
import asyncio
import math
import time

import flet as ft

from Catalog import BY_KEY, TYPES
from DemoService import DemoService
from Meter import TARGETS
from Store import Store


INK = "#16343E"
MUTED = "#647C83"
BLUE = "#087F78"
BACKGROUND = "#F3F7F5"
CARD = "#FFFFFF"
DISABLED = "#E9EFED"
WARNING = "#FFF3DB"
GOOD = "#198468"
HIGH = "#C7664C"
LOW = "#B67A24"
BORDER = "#E3EBE8"
STALENESS_SECONDS = 15


class ControlApp:
    def __init__(self, page: ft.Page, demo: bool):
        self.page = page
        self.demo = demo
        self.store = Store()
        self.section = "Dashboard"
        self.controller = None
        self.candidates = []
        self.devices = []
        self.readings = {}
        self.state = {}
        self.last_state = 0.0
        self.busy = False
        self.reconnecting = False
        self.auto_reconnect = False
        self.reconnect_task = None
        self.closed = False
        self.connection_lock = asyncio.Lock()
        self.subscription_lock = asyncio.Lock()
        self.passkey = ft.TextField(label="BLE passkey from ESP serial monitor", width=300, max_length=6)
        self.ssid = ft.TextField(label="WiFi network name (SSID)", width=420)
        self.password = ft.TextField(label="WiFi password", password=True, can_reveal_password=True, width=420)
        if demo:
            self.service = DemoService(self._state_received, self._telemetry_received, self._disconnected)
        else:
            from BleService import BleService
            self.service = BleService(self._state_received, self._telemetry_received, self._disconnected)
        self.page.title = "Hydroponics Control"
        self.page.bgcolor = BACKGROUND
        self.page.padding = 24
        self.page.on_close = self._close
        self.render()
        self.page.run_task(self._watch_state)

    @property
    def connected(self):
        return self.service.connected

    @property
    def state_fresh(self):
        return self.connected and time.monotonic() - self.last_state < STALENESS_SECONDS

    @property
    def low_power(self):
        return self.state_fresh and self.state.get("mode") == "low_power"

    def _paused(self, kind):
        allowed = self.state.get("active_types", [item.key for item in TYPES if item.essential])
        return self.low_power and kind.key not in allowed

    def _state_received(self, message):
        if self.closed:
            return
        old = (self.state.get("mode"), self.state.get("wifi"), self.state_fresh)
        self.state = message
        self.last_state = time.monotonic()
        new = (self.state.get("mode"), self.state.get("wifi"), self.state_fresh)
        if old != new or self.section == "Dashboard":
            self.render()

    def _telemetry_received(self, message):
        if self.closed or self.section != "Dashboard":
            return
        try:
            value = float(message["value"])
            if not math.isfinite(value):
                return
            node = str(message["node"])[:80]
            device = str(message["device"])[:120]
            metric = str(message["metric"])[:80]
            unit = str(message.get("unit", ""))[:20]
        except (KeyError, ValueError, TypeError):
            return
        self.store.record(node, device, metric, value, unit)
        self.readings[(device, metric)] = (value, unit, time.monotonic())
        if self.section == "Dashboard":
            self.render()

    def _disconnected(self, reason="BLE connection lost"):
        if self.closed:
            return
        self.state = {}
        self.last_state = 0.0
        self.devices = []
        self.readings.clear()
        if self.auto_reconnect and self.controller and not self.closed:
            self.notice("BLE connection lost. Trying to reconnect…")
            if self.reconnect_task is None or self.reconnect_task.done():
                self.reconnect_task = self.page.run_task(self._reconnect)
        else:
            self.notice(reason)
        self.render()

    async def _reconnect(self):
        self.reconnecting = True
        self.render()
        try:
            for delay in (2, 4, 8):
                await asyncio.sleep(delay)
                if not self.auto_reconnect or self.closed or self.connected:
                    return
                try:
                    async with self.connection_lock:
                        if not self.auto_reconnect or self.closed:
                            return
                        candidates = await self.service.scan()
                        match = next((item for item in candidates
                                      if item.address == self.controller.address), None)
                        if match is None:
                            continue
                        await self.service.connect(match, "")
                        await self.service.set_telemetry_enabled(self.section == "Dashboard")
                        if self.section == "Sensors & devices":
                            await self._load_devices()
                        self.controller = match
                    self.notice("Controller reconnected.")
                    return
                except ValueError:
                    break  # A lost bond needs a fresh passkey in Connect.
                except Exception:
                    try:
                        await self.service.disconnect()
                    except Exception:
                        pass  # The next attempt creates a new BLE client.
            if self.auto_reconnect and not self.closed:
                self.notice("Automatic reconnect stopped. Open Connect to try again.")
        finally:
            self.reconnecting = False
            if not self.closed:
                self.render()

    async def _watch_state(self):
        while not self.closed:
            was_fresh = self.state_fresh
            await asyncio.sleep(2)
            if self.closed:
                return
            if was_fresh and not self.state_fresh:
                self.render()

    def notice(self, message):
        self.page.show_dialog(ft.SnackBar(ft.Text(message)))

    def _navigate(self, section):
        self.section = section
        self.render()
        if self.connected:
            self.page.run_task(self._sync_section)

    async def _sync_section(self):
        try:
            async with self.subscription_lock:
                await self.service.set_telemetry_enabled(self.section == "Dashboard")
                if self.section == "Sensors & devices":
                    await self._load_devices()
                    self.render()
        except Exception as error:
            self.notice(f"Could not update this page: {error}")

    async def _close(self, _event):
        self.closed = True
        self.auto_reconnect = False
        if self.reconnect_task and not self.reconnect_task.done():
            self.reconnect_task.cancel()
        async with self.connection_lock:
            await self.service.disconnect()
        self.store.close()

    def _card(self, controls, disabled=False, width=None):
        return ft.Container(
            content=ft.Column(controls=controls, spacing=12),
            bgcolor=DISABLED if disabled else CARD,
            padding=20, border_radius=18, width=width, disabled=disabled,
        )

    def _pill(self, label, color, background):
        return ft.Container(
            content=ft.Text(label, size=12, weight=ft.FontWeight.BOLD, color=color),
            bgcolor=background, padding=ft.Padding.symmetric(horizontal=11, vertical=7),
            border_radius=30,
        )

    def _status(self):
        if self.reconnecting:
            return "Reconnecting to controller", LOW
        if not self.connected:
            return "Controller offline", MUTED
        if not self.state_fresh:
            return "State stale · controls paused", HIGH
        if self.low_power:
            return "Low power · WiFi off", LOW
        return f"Connected · {self.state.get('node', 'Controller')}", GOOD

    def _header(self):
        label, color = self._status()
        return ft.Row(controls=[
            ft.Container(
                content=ft.Icon(ft.Icons.WATER_DROP_ROUNDED, color=CARD, size=30),
                bgcolor=BLUE, width=54, height=54, border_radius=16,
                alignment=ft.Alignment.CENTER,
            ),
            ft.Column(controls=[
                ft.Text("Hydroponics Control", size=26, weight=ft.FontWeight.BOLD, color=INK),
                ft.Text("Live monitoring · local data · secure setup", size=12, color=MUTED),
            ], expand=True, spacing=2),
            self._pill(label, color, "#E6F4EF" if color == GOOD else WARNING if color == LOW else DISABLED),
        ], spacing=14, wrap=True)

    def _navigation(self):
        tabs = (
            ("Dashboard", ft.Icons.DASHBOARD_ROUNDED),
            ("Connect", ft.Icons.BLUETOOTH_SEARCHING),
            ("WiFi configuration", ft.Icons.WIFI_ROUNDED),
            ("Sensors & devices", ft.Icons.TUNE_ROUNDED),
        )
        return ft.Row(controls=[
            ft.Button(content=title, icon=icon,
                      on_click=lambda e, name=title: self._navigate(name),
                      bgcolor=BLUE if self.section == title else CARD,
                      color=CARD if self.section == title else INK)
            for title, icon in tabs
        ], wrap=True, spacing=8)

    def render(self):
        body = {
            "Dashboard": self._dashboard,
            "Connect": self._connect_page,
            "WiFi configuration": self._wifi_page,
            "Sensors & devices": self._devices_page,
        }[self.section]()
        self.page.clean()
        self.page.add(ft.Column(
            controls=[self._header(), self._navigation(), ft.Divider(color=BORDER), body],
            spacing=18, scroll=ft.ScrollMode.AUTO, expand=True,
        ))
        self.page.update()

    def _meter(self, device, metric, value, unit, stamp):
        kind = next((item for item in TYPES if item.label == metric), None)
        target = TARGETS.get(kind.key) if kind else None
        paused = bool(kind and self._paused(kind))
        fresh = time.monotonic() - stamp < STALENESS_SECONDS and not paused
        status = "Paused" if paused else "Stale" if not fresh else target.status(value) if target else "Live"
        color = MUTED if not fresh or not target else GOOD if status == "Normal" else LOW if status == "Low" else HIGH
        progress = target.fraction(value) if target and fresh else 0.0
        display_value = f"{value:g}" if fresh else "—"
        icon = {
            "level": ft.Icons.WATER_DROP_ROUNDED,
            "ph": ft.Icons.SCIENCE_ROUNDED,
            "tds": ft.Icons.GRAIN_ROUNDED,
            "climate": ft.Icons.THERMOSTAT_ROUNDED,
            "light": ft.Icons.WB_SUNNY_ROUNDED,
            "gas": ft.Icons.AIR_ROUNDED,
        }.get(kind.key if kind else "", ft.Icons.SENSORS_ROUNDED)
        ring = ft.Stack(width=116, height=116, alignment=ft.Alignment.CENTER, controls=[
            ft.ProgressRing(value=progress, width=112, height=112, stroke_width=10,
                            color=color, bgcolor=DISABLED, semantics_label=f"{metric}: {status}"),
            ft.Container(width=112, height=112, alignment=ft.Alignment.CENTER,
                         content=ft.Column(controls=[
                             ft.Text(display_value, size=23, weight=ft.FontWeight.BOLD, color=color),
                             ft.Text(unit if fresh else "offline", size=11, color=MUTED),
                         ], spacing=0, alignment=ft.MainAxisAlignment.CENTER,
                            horizontal_alignment=ft.CrossAxisAlignment.CENTER)),
        ])
        return ft.Container(
            content=ft.Column(controls=[
                ft.Row(controls=[ft.Icon(icon, color=BLUE, size=21),
                                 ft.Text(metric, size=16, weight=ft.FontWeight.BOLD, color=INK, expand=True)],
                       spacing=8),
                ft.Row(controls=[ring, ft.Column(controls=[
                    self._pill(status.upper(), color, DISABLED if not fresh else "#E8F4EF" if status == "Normal" else WARNING if status == "Low" else "#FBECE7"),
                    ft.Text(target.caption if target else "No target set for this sensor",
                            size=11, color=MUTED, width=145),
                    ft.Text("Updated just now" if fresh else "Awaiting active readings",
                            size=11, color=MUTED),
                ], spacing=9)], spacing=16),
                ft.Text(device, size=10, color=MUTED),
            ], spacing=12),
            bgcolor=CARD, border_radius=18, padding=18, width=324,
        )

    def _dashboard(self):
        if not self.connected:
            return self._card([
                ft.Icon(ft.Icons.BLUETOOTH_SEARCHING, color=BLUE, size=36),
                ft.Text("Connect your controller", size=22, weight=ft.FontWeight.BOLD, color=INK),
                ft.Text("Scan nearby ESP32 controllers to begin monitoring your system.", color=MUTED),
                ft.Button("Find a controller", icon=ft.Icons.BLUETOOTH_SEARCHING,
                          on_click=lambda e: self._navigate("Connect"), bgcolor=BLUE, color=CARD),
            ])
        metrics = [self._meter(device, metric, value, unit, stamp)
                   for (device, metric), (value, unit, stamp) in sorted(self.readings.items())]
        if not metrics:
            metrics = [self._card([ft.Text("Waiting for sensor readings…", color=MUTED)])]
        history = [ft.Text("Recent local readings", size=17, weight=ft.FontWeight.BOLD, color=INK)]
        for received, node, device, metric, value, unit in self.store.recent(8):
            history.append(ft.Text(f"{received[11:19]} UTC  ·  {metric}: {value:g} {unit}  ·  {node}",
                                   size=12, color=MUTED))
        return ft.Column(controls=[
            ft.Row(controls=[
                self._card([ft.Text("CONTROLLER MODE", size=11, color=MUTED, weight=ft.FontWeight.BOLD),
                            ft.Text("Low power" if self.low_power else self.state.get("mode", "Unknown").replace("_", " ").title(),
                                    size=21, color=INK, weight=ft.FontWeight.BOLD)], width=220),
                self._card([ft.Text("WIFI STATUS", size=11, color=MUTED, weight=ft.FontWeight.BOLD),
                            ft.Text(str(self.state.get("wifi", "Unknown")).title(),
                                    size=21, color=INK, weight=ft.FontWeight.BOLD)], width=220),
                self._card([ft.Text("LOCAL READINGS", size=11, color=MUTED, weight=ft.FontWeight.BOLD),
                            ft.Text(str(self.store.count()), size=21, color=INK,
                                    weight=ft.FontWeight.BOLD)], width=220),
            ], wrap=True, spacing=12),
            ft.Row(controls=[ft.Text("Live sensors", size=22, weight=ft.FontWeight.BOLD, color=INK),
                             self._pill("SIMULATED DEMO RANGES", BLUE, "#E7F3F1")],
                   wrap=True, spacing=12),
            ft.Text("Ring position shows the reading across its display scale. Color and label show the demo target status.",
                    color=MUTED, size=12),
            ft.Row(controls=metrics, wrap=True, spacing=14),
            self._card(history),
        ], spacing=16)

    def _connect_page(self):
        rows = [
            ft.Text("Find a controller", size=21, weight=ft.FontWeight.BOLD, color=INK),
            ft.Text("Enter the six-digit code printed by the ESP serial monitor for first pairing. Leave it blank if this laptop is already paired."),
            self.passkey,
            ft.Button("Scan nearby BLE controllers", on_click=self._scan, disabled=self.busy),
        ]
        for controller in self.candidates:
            rows.append(ft.Row(controls=[
                ft.Column(controls=[ft.Text(controller.label, weight=ft.FontWeight.BOLD),
                                    ft.Text(controller.address, size=12, color=MUTED)], expand=True),
                ft.Button("Connect", on_click=lambda e, item=controller: self.page.run_task(self._connect, item),
                          disabled=self.busy),
            ]))
        if self.connected:
            rows.append(ft.Button("Disconnect", on_click=self._disconnect))
        if self.demo:
            rows.extend([ft.Divider(), ft.Text("Demo controls", weight=ft.FontWeight.BOLD),
                         ft.Button("Toggle low-power mode", on_click=self._toggle_demo, disabled=not self.connected)])
        return self._card(rows)

    def _wifi_page(self):
        blocked = not self.state_fresh or self.low_power
        reason = ("Low-power mode is active. WiFi is switched off on the controller."
                  if self.low_power else "Connect to a controller with a current state to configure WiFi.")
        return ft.Column(controls=[
            ft.Container(content=ft.Text(reason, color="#805600", weight=ft.FontWeight.BOLD),
                         bgcolor=WARNING, padding=12, border_radius=10) if blocked else ft.Text("Send new WiFi settings securely over BLE."),
            self._card([
                ft.Text("WiFi configuration", size=21, weight=ft.FontWeight.BOLD, color=INK),
                ft.Text("The password is sent once and is not saved on this laptop.", color=MUTED),
                self.ssid, self.password,
                ft.Button("Send WiFi settings", on_click=self._send_wifi, disabled=blocked or self.busy),
            ], disabled=blocked),
        ])

    def _devices_page(self):
        blocked = not self.state_fresh
        rows = [
            ft.Text("Sensors & devices", size=21, weight=ft.FontWeight.BOLD, color=INK),
            ft.Text("Choose from supported hardware. The controller validates every change.", color=MUTED),
            ft.Text("Available to add", size=17, weight=ft.FontWeight.BOLD),
        ]
        configured_types = {item.get("type") for item in self.devices}
        for kind in TYPES:
            paused = self._paused(kind)
            rows.append(ft.Container(
                content=ft.Row(controls=[
                    ft.Column(controls=[ft.Text(kind.label, weight=ft.FontWeight.BOLD,
                                                color=MUTED if paused else INK),
                                        ft.Text("Paused in low-power mode" if paused else kind.profile,
                                                size=12, color=MUTED)], expand=True),
                    ft.Button("Add", on_click=lambda e, item=kind: self.page.run_task(self._add_device, item),
                              disabled=blocked or paused or kind.key in configured_types or self.busy),
                ]), bgcolor=DISABLED if paused else BACKGROUND, padding=10, border_radius=10,
            ))
        rows.extend([
            ft.Divider(),
            ft.Row(controls=[ft.Text("Configured on controller", size=17, weight=ft.FontWeight.BOLD, expand=True),
                             ft.Button("Refresh", on_click=self._refresh_devices, disabled=blocked or self.busy)]),
        ])
        if not self.devices:
            rows.append(ft.Text("No devices reported. Connect and refresh the list.", color=MUTED))
        for device in self.devices:
            kind = BY_KEY.get(device.get("type", ""))
            grey = self._paused(kind) if kind else self.low_power
            label = device.get("label") or (kind.label if kind else device.get("id", "Unknown"))
            subtitle = "Paused in low-power mode" if grey else ("Online" if device.get("online") else "Configured · presence unverified")
            rows.append(ft.Container(
                content=ft.Row(controls=[
                    ft.Column(controls=[ft.Text(label, weight=ft.FontWeight.BOLD, color=MUTED if grey else INK),
                                        ft.Text(subtitle, color=MUTED, size=12)], expand=True),
                    ft.Button("Remove", on_click=lambda e, item=device: self._confirm_delete(item),
                              disabled=blocked or grey or self.busy),
                ]), bgcolor=DISABLED if grey else BACKGROUND, padding=12, border_radius=10,
            ))
        if self.low_power:
            rows.insert(0, ft.Container(content=ft.Text("Low power: air climate, light, gas and relay setup are paused. Reservoir level, pH and TDS remain available.", color="#805600"),
                                        bgcolor=WARNING, padding=12, border_radius=10))
        elif blocked:
            rows.insert(0, ft.Container(content=ft.Text("Controller state is unavailable or stale. Changes are paused.", color="#805600"),
                                        bgcolor=WARNING, padding=12, border_radius=10))
        return self._card(rows)

    async def _scan(self, _event):
        self.busy = True
        self.render()
        try:
            self.candidates = await self.service.scan()
            self.notice(f"Found {len(self.candidates)} compatible controller(s).")
        except Exception as error:
            self.notice(f"BLE scan failed: {error}")
        finally:
            self.busy = False
            self.render()

    async def _connect(self, controller):
        self.auto_reconnect = False
        if self.reconnect_task and not self.reconnect_task.done():
            self.reconnect_task.cancel()
        self.busy = True
        self.render()
        try:
            async with self.connection_lock:
                await self.service.connect(controller, (self.passkey.value or "").strip())
                await self.service.set_telemetry_enabled(True)
            self.passkey.value = ""
            self.controller = controller
            self.devices = []
            self.readings.clear()
            self.section = "Dashboard"
            self.auto_reconnect = True
            self.notice(f"Connected to {controller.label}.")
        except Exception as error:
            try:
                await self.service.disconnect()
            except Exception:
                pass
            self.notice(f"Connection failed: {error}")
        finally:
            self.busy = False
            self.render()

    async def _disconnect(self, _event):
        self.auto_reconnect = False
        if self.reconnect_task and not self.reconnect_task.done():
            self.reconnect_task.cancel()
        async with self.connection_lock:
            await self.service.disconnect()
        self.controller = None
        self._disconnected("Disconnected from controller.")

    async def _load_devices(self):
        result = await self.service.send("get_devices", {})
        self.devices = result.get("devices", [])

    async def _refresh_devices(self, _event):
        try:
            await self._load_devices()
            self.render()
        except Exception as error:
            self.notice(f"Could not refresh devices: {error}")

    async def _send_wifi(self, _event):
        if not self.state_fresh or self.low_power:
            return
        name, password = (self.ssid.value or "").strip(), self.password.value or ""
        if not name or len(name.encode("utf-8")) > 32 or len(password) < 8 or len(password) > 63:
            self.notice("Enter an SSID up to 32 bytes and a password of 8–63 characters.")
            return
        try:
            await self.service.send("set_wifi", {"ssid": name, "password": password})
            self.notice("WiFi settings accepted by controller.")
        except Exception:
            self.notice("WiFi settings were not accepted. Check the connection and try again.")
        finally:
            self.password.value = ""
            self.password.update()

    async def _add_device(self, kind):
        if not self.state_fresh:
            return
        if self._paused(kind):
            self.notice("This device is unavailable in low-power mode.")
            return
        try:
            await self.service.send("add_device", {"type": kind.key, "label": kind.label})
            await self._load_devices()
            self.notice(f"{kind.label} added.")
            self.render()
        except Exception as error:
            self.notice(f"Could not add device: {error}")

    def _confirm_delete(self, device):
        label = device.get("label", device.get("id", "device"))
        async def remove(_event):
            self.page.pop_dialog()
            try:
                await self.service.send("delete_device", {"id": device["id"]})
                self.readings = {
                    key: value for key, value in self.readings.items()
                    if key[0] != device["id"]
                }
                await self._load_devices()
                self.notice(f"{label} removed.")
                self.render()
            except Exception as error:
                self.notice(f"Could not remove device: {error}")
        self.page.show_dialog(ft.AlertDialog(
            modal=True, title=ft.Text(f"Remove {label}?"),
            content=ft.Text("The controller will stop using this configured device."),
            actions=[ft.TextButton("Cancel", on_click=lambda e: self.page.pop_dialog()),
                     ft.Button("Remove device", on_click=remove)],
        ))

    def _toggle_demo(self, _event):
        self.service.set_low_power(not self.service.low_power)
        self.render()


def main(page: ft.Page):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--demo", action="store_true")
    arguments, _ = parser.parse_known_args()
    ControlApp(page, arguments.demo)


if __name__ == "__main__":
    ft.run(main)

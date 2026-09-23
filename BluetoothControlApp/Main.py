"""Hydroponics controller desktop app. Run with --demo for a local simulator."""

import argparse
import asyncio
import math
import time

import flet as ft

from Catalog import BY_KEY, TYPES
from DemoService import DemoService
from Store import Store


INK = "#17324B"
MUTED = "#617286"
BLUE = "#0E7490"
BACKGROUND = "#F4F8FA"
CARD = "#FFFFFF"
DISABLED = "#E9EDF0"
WARNING = "#FFF2CF"
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
        self.subscription_lock = asyncio.Lock()
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
        old = (self.state.get("mode"), self.state.get("wifi"), self.state_fresh)
        self.state = message
        self.last_state = time.monotonic()
        new = (self.state.get("mode"), self.state.get("wifi"), self.state_fresh)
        if old != new or self.section == "Dashboard":
            self.render()

    def _telemetry_received(self, message):
        if self.section != "Dashboard":
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
        self.state = {}
        self.last_state = 0.0
        self.devices = []
        self.readings.clear()
        self.render()
        self.notice(reason)

    async def _watch_state(self):
        while True:
            was_fresh = self.state_fresh
            await asyncio.sleep(2)
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
        await self.service.disconnect()
        self.store.close()

    def _card(self, controls, disabled=False, width=None):
        return ft.Container(
            content=ft.Column(controls=controls, spacing=12),
            bgcolor=DISABLED if disabled else CARD,
            padding=20, border_radius=14, width=width, disabled=disabled,
        )

    def _status(self):
        if not self.connected:
            return "Disconnected", MUTED
        if not self.state_fresh:
            return "State stale — controls paused", "#A33C2B"
        if self.low_power:
            return "LOW POWER · WiFi off", "#9B6500"
        return f"Connected · {self.state.get('node', 'Controller')}", BLUE

    def _header(self):
        label, color = self._status()
        badge = self._card([
            ft.Text(label, color=color, weight=ft.FontWeight.BOLD),
            ft.Text("Demo controller" if self.demo else "BLE controller", size=12, color=MUTED),
        ])
        return ft.Row(controls=[
            ft.Column(controls=[
                ft.Text("Hydroponics Control", size=27, weight=ft.FontWeight.BOLD, color=INK),
                ft.Text("Local monitoring and controller setup", color=MUTED),
            ], expand=True), badge,
        ])

    def _navigation(self):
        return ft.Row(controls=[
            ft.Button(content=title, on_click=lambda e, name=title: self._navigate(name),
                      bgcolor=BLUE if self.section == title else CARD,
                      color=CARD if self.section == title else INK)
            for title in ("Dashboard", "Connect", "WiFi configuration", "Sensors & devices")
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
            controls=[self._header(), self._navigation(), ft.Divider(), body],
            spacing=16, scroll=ft.ScrollMode.AUTO, expand=True,
        ))
        self.page.update()

    def _dashboard(self):
        if not self.connected:
            return self._card([
                ft.Text("No controller connected", size=20, weight=ft.FontWeight.BOLD),
                ft.Text("Open Connect to find your ESP32 controller."),
                ft.Button("Connect a controller", on_click=lambda e: self._navigate("Connect")),
            ])
        metrics = []
        for (device, metric), (value, unit, stamp) in sorted(self.readings.items()):
            kind = next((item for item in TYPES if item.label == metric), None)
            paused = bool(kind and self._paused(kind))
            fresh = time.monotonic() - stamp < 15 and not paused
            value_text = "Paused in low-power mode" if paused else (
                f"{value:g} {unit}" if fresh else "No recent reading"
            )
            metrics.append(self._card([
                ft.Text(metric, color=MUTED),
                ft.Text(value_text, size=23,
                        weight=ft.FontWeight.BOLD, color=INK if fresh else MUTED),
                ft.Text(device, size=11, color=MUTED),
            ], disabled=not fresh, width=235))
        if not metrics:
            metrics = [self._card([ft.Text("Waiting for sensor readings…", color=MUTED)])]
        history = [ft.Text("Latest readings saved on this laptop", size=17, weight=ft.FontWeight.BOLD)]
        for received, node, device, metric, value, unit in self.store.recent(8):
            history.append(ft.Text(f"{received[11:19]} UTC  ·  {metric}: {value:g} {unit}  ·  {node}"))
        return ft.Column(controls=[
            ft.Row(controls=[
                self._card([ft.Text("Controller mode", color=MUTED),
                            ft.Text("Low power" if self.low_power else self.state.get("mode", "Unknown").replace("_", " ").title(),
                                    size=20, weight=ft.FontWeight.BOLD)]),
                self._card([ft.Text("WiFi", color=MUTED),
                            ft.Text(str(self.state.get("wifi", "Unknown")).title(), size=20, weight=ft.FontWeight.BOLD)]),
                self._card([ft.Text("Readings stored locally", color=MUTED),
                            ft.Text(str(self.store.count()), size=20, weight=ft.FontWeight.BOLD)]),
            ], wrap=True, spacing=12),
            ft.Text("Live sensors", size=20, weight=ft.FontWeight.BOLD, color=INK),
            ft.Row(controls=metrics, wrap=True, spacing=12),
            self._card(history),
        ], spacing=16)

    def _connect_page(self):
        rows = [
            ft.Text("Find a controller", size=21, weight=ft.FontWeight.BOLD, color=INK),
            ft.Text("Select the controller you are physically commissioning. Pairing is handled by Ubuntu."),
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
        self.busy = True
        self.render()
        try:
            await self.service.connect(controller)
            self.controller = controller
            self.devices = []
            self.readings.clear()
            self.section = "Dashboard"
            await self.service.set_telemetry_enabled(True)
            self.notice(f"Connected to {controller.label}.")
        except Exception as error:
            self.notice(f"Connection failed: {error}")
        finally:
            self.busy = False
            self.render()

    async def _disconnect(self, _event):
        await self.service.disconnect()
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

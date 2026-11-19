"""Connection: Bluetooth discovery, connect / disconnect, demo switches.

The link is open: no passkey and no pairing step. Scan, then Connect.

Bluetooth is the only link to the controller. Wi-Fi is a controller setting
on the Wi-Fi page, not a way to connect.
"""

import flet as ft

import Theme


class ConnectionPage:
    name = "Connection"

    def __init__(self, app):
        self.app = app
        self.targets = []
        self.scanning = False
        self.busy = False
        self.links = ft.Column(spacing=8)
        self.results = ft.Column(spacing=8)
        self.scan_button = ft.FilledButton("Scan for controllers", icon=ft.Icons.BLUETOOTH_SEARCHING_ROUNDED,
                                           on_click=lambda e: self.app.run(self._scan))
        self.scan_ring = ft.ProgressRing(width=18, height=18, stroke_width=2, visible=False)
        self.actions = ft.Row(spacing=8, wrap=True)
        self.root = None

    def build(self) -> ft.Control:
        s = self.app.session
        sections = [
            Theme.title("Connection"),
            Theme.card([Theme.title("Status", 17), self.links, self.actions]),
            Theme.card([
                Theme.title("Find a controller", 17),
                Theme.caption("The app talks to the controller over Bluetooth only. Keep the ESP powered and "
                              "within range, scan, then choose Connect. No passkey or pairing is needed. "
                              "The controller serves one computer at a time."),
                ft.Row([self.scan_button, self.scan_ring], spacing=12),
                self.results,
            ]),
        ]
        if s.demo:
            c = s.demo_controller
            sections.append(Theme.card([
                Theme.title("Demo controls", 17),
                Theme.caption("Simulate failures to see reconnects, low-power mode and alarms."),
                ft.Switch(label="Controller in Bluetooth range", value=c.ble_in_range,
                          on_change=lambda e: c.set_ble_in_range(bool(e.control.value))),
                ft.Switch(label="External 5 V present (off = low-power mode)", value=c.external_power,
                          on_change=lambda e: c.set_external_power(bool(e.control.value))),
                ft.Switch(label="Controller Wi-Fi outage", value=c.wifi_outage,
                          on_change=lambda e: c.set_wifi_outage(bool(e.control.value))),
                ft.Switch(label="Over-temperature (trips latching rule 0)", value=c.heat,
                          on_change=lambda e: c.set_heat(bool(e.control.value))),
            ]))
        self.root = ft.Column(sections, spacing=16)
        self.refresh("all")
        return self.root

    def needs_rebuild(self) -> bool:
        return False

    def refresh(self, kind: str) -> list:
        if self.root is None:
            return []
        s = self.app.session
        if s.ble.connected:
            text, color = f"Connected to {s.ble_target.label if s.ble_target else 'controller'}", Theme.GOOD
        elif s.ble_reconnecting:
            text, color = "Reconnecting…", Theme.WARN
        else:
            text, color = "Not connected", Theme.NEUTRAL
        self.links.controls = [
            ft.Row([ft.Icon(ft.Icons.BLUETOOTH_ROUNDED, color=color),
                    ft.Text("Bluetooth", weight=ft.FontWeight.W_600, width=90),
                    ft.Text(text, color=Theme.MUTED, expand=True)],
                   vertical_alignment=ft.CrossAxisAlignment.CENTER),
        ]
        actions = []
        if s.connected or s.ble_reconnecting:
            actions.append(ft.OutlinedButton("Disconnect", icon=ft.Icons.LINK_OFF_ROUNDED,
                                             on_click=lambda e: self.app.run(self._disconnect)))
        self.actions.controls = actions
        self.scan_button.disabled = self.scanning or self.busy
        self.scan_ring.visible = self.scanning
        self._paint_results()
        return [self.root]

    def _paint_results(self) -> None:
        rows = []
        for target in self.targets:
            detail = target.address
            if target.rssi is not None:
                detail += f" · signal {target.rssi} dBm"
            rows.append(ft.Container(
                content=ft.Row([
                    ft.Icon(ft.Icons.BLUETOOTH_ROUNDED, color=ft.Colors.PRIMARY),
                    ft.Column([ft.Text(target.label, weight=ft.FontWeight.W_600), Theme.caption(detail)],
                              spacing=2, expand=True),
                    ft.FilledButton("Connect", disabled=self.busy,
                                    on_click=lambda e, t=target: self.app.run(self._connect, t)),
                ], vertical_alignment=ft.CrossAxisAlignment.CENTER),
                bgcolor=Theme.tint(Theme.NEUTRAL, 0.06), border_radius=12, padding=12,
            ))
        if not rows and not self.scanning:
            rows.append(Theme.caption("No controllers listed yet. Scan with the ESP powered and nearby."))
        self.results.controls = rows

    async def _scan(self) -> None:
        self.scanning = True
        self.app.refresh_page()
        try:
            self.targets = await self.app.session.scan()
            if not self.targets:
                self.app.notice("No controller found. Check that it is powered and close by, and that no other "
                                "computer is connected to it (it serves one at a time).")
        finally:
            self.scanning = False
            self.app.refresh_page()

    async def _connect(self, target) -> None:
        self.busy = True
        self.app.refresh_page()
        try:
            await self.app.session.connect_ble(target)
            self.app.notice(f"Connected to {target.label} over Bluetooth.")
            self.app.navigate("Dashboard")
        except Exception as error:
            self.app.notice(self.app.explain(error, "Connection failed"))
        finally:
            self.busy = False
            self.app.refresh_page()

    async def _disconnect(self) -> None:
        await self.app.session.disconnect()
        self.app.notice("Disconnected.")

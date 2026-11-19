"""Wi-Fi: the controller's own network setting, sent over Bluetooth.

The app never connects over Wi-Fi: this page only shows and sets the network
the controller joins (for future server publishing). Automatic Wi-Fi shutdown
and restart depend on the firmware's power-transition policy.
"""

import flet as ft

import Theme

STATE_TEXT = {
    "connected": ("Connected", Theme.GOOD),
    "connecting": ("Connecting…", Theme.WARN),
    "failed": ("Could not connect", Theme.BAD),
    "unconfigured": ("Not set up", Theme.NEUTRAL),
}


class WifiPage:
    name = "Wi-Fi"

    def __init__(self, app):
        self.app = app
        self.ssid = ft.TextField(label="Network name (SSID)", width=380, max_length=32)
        self.password = ft.TextField(label="Password", password=True, can_reveal_password=True, width=380,
                                     max_length=63, on_submit=lambda e: self.app.run(self._send))
        self.status = ft.Column(spacing=6)
        self.banner = ft.Column([])
        self.send = ft.FilledButton("Send to controller", icon=ft.Icons.SEND_ROUNDED,
                                    on_click=lambda e: self.app.run(self._send))
        self.forget = ft.OutlinedButton("Forget Wi-Fi", icon=ft.Icons.WIFI_OFF_ROUNDED,
                                        on_click=lambda e: self.app.run(self._forget))
        self.busy = False
        self.root = None

    def build(self) -> ft.Control:
        self.root = ft.Column([
            Theme.title("Wi-Fi"),
            self.banner,
            Theme.card([Theme.title("Controller network", 17), self.status]),
            Theme.card([
                Theme.title("Set up Wi-Fi", 17),
                Theme.caption("The password is sent over unencrypted Bluetooth and stored only on the controller."),
                self.ssid, self.password,
                ft.Row([self.send, self.forget], spacing=10, wrap=True),
            ]),
        ], spacing=16)
        self.refresh("all")
        return self.root

    def needs_rebuild(self) -> bool:
        return False

    def refresh(self, kind: str) -> list:
        if self.root is None:
            return []
        s = self.app.session
        wifi = (s.state or {}).get("wifi", {})
        label, color = STATE_TEXT.get(wifi.get("state", ""), ("Unknown", Theme.NEUTRAL))
        rows = [ft.Row([Theme.chip(label, color, ft.Icons.WIFI_ROUNDED)], spacing=8)]
        if wifi.get("ssid"):
            rows.append(Theme.caption(f"Network: {wifi['ssid']}"))
        if wifi.get("ip"):
            rssi = f" · signal {wifi['rssi']} dBm" if wifi.get("rssi") is not None else ""
            rows.append(Theme.caption(f"Address: {wifi['ip']}{rssi}"))
        if s.power_mode in ("low_power", "transition_to_low"):
            rows.append(Theme.caption("In low power, automatic Wi-Fi shutdown depends on the firmware configuration."))
        if not s.state:
            rows = [Theme.caption("Connect to a controller to see its network status.")]
        self.status.controls = rows
        blocked = not s.can_command
        self.banner.controls = [Theme.banner("Changing Wi-Fi settings needs a current Bluetooth connection.")] \
            if blocked else []
        for control in (self.send, self.forget, self.ssid, self.password):
            control.disabled = blocked or self.busy
        self.forget.disabled = self.forget.disabled or wifi.get("state") == "unconfigured"
        return [self.root]

    async def _run(self, action, done: str) -> None:
        self.busy = True
        self.app.refresh_page()
        try:
            await action()
            self.app.notice(done)
        except Exception as error:
            self.app.notice(self.app.explain(error))
        finally:
            self.busy = False
            self.app.refresh_page()

    async def _send(self) -> None:
        ssid, password = (self.ssid.value or "").strip(), self.password.value or ""
        if not ssid or len(ssid.encode("utf-8")) > 32:
            self.app.notice("Enter a network name of up to 32 bytes.")
            return
        if not 8 <= len(password) <= 63 or not password.isascii() or not password.isprintable():
            self.app.notice("The password must be 8–63 printable ASCII characters.")
            return
        self.password.value = ""
        await self._run(lambda: self.app.session.set_wifi(ssid, password),
                        "Wi-Fi settings saved on the controller. Watch the status above.")

    async def _forget(self) -> None:
        await self._run(self.app.session.forget_wifi, "The controller forgot its Wi-Fi network.")

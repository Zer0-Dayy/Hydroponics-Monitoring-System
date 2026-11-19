"""Sensors: the board's fixed sensors with live health and an on/off switch.

Switching needs Bluetooth. The controller persists the choice, applies it at
a safe point and only then answers; it refuses to switch off a sensor whose
alarm rule is latched or active.
"""

import flet as ft

import Theme
from Catalog import metric_label


class SensorRow:
    def __init__(self, page, sensor):
        self.page = page
        self.sensor = sensor
        self.switch = ft.Switch(on_change=self._changed)
        self.pending = ft.ProgressRing(width=18, height=18, stroke_width=2, visible=False)
        self.chips = ft.Row(wrap=True, spacing=6, run_spacing=6)
        self.note = Theme.caption("")
        self.control = Theme.card([
            ft.Row([
                ft.Column([ft.Text(sensor.label, size=15, weight=ft.FontWeight.W_600, color=Theme.TEXT),
                           Theme.caption(", ".join(metric_label(m) for m in sensor.metrics))],
                          spacing=2, expand=True),
                self.pending, self.switch,
            ], vertical_alignment=ft.CrossAxisAlignment.CENTER),
            self.chips,
            self.note,
        ], spacing=8)
        self.busy = False

    def paint(self) -> None:
        s = self.page.app.session
        enabled = self.sensor.key in s.enabled
        if not self.busy:
            self.switch.value = enabled
        self.switch.disabled = self.busy or not s.can_command
        self.pending.visible = self.busy
        chips = []
        for metric in self.sensor.metrics:
            reading = s.readings.get(metric)
            health = "DISABLED" if not enabled else reading["health"] if reading else "NO_DATA"
            label, color = Theme.HEALTH_STYLE.get(health, (health, Theme.NEUTRAL))
            chips.append(Theme.chip(f"{metric_label(metric)}: {label}", color))
        self.chips.controls = chips
        self.note.value = f"If switched off: {self.sensor.depends_note}" if self.sensor.depends_note else ""
        self.note.visible = bool(self.sensor.depends_note)

    def _changed(self, event) -> None:
        wanted = bool(event.control.value)
        if not wanted and self.sensor.depends_note:
            self.page.confirm_disable(self, wanted)
        else:
            self.page.app.run(self.apply, wanted)

    async def apply(self, wanted: bool) -> None:
        s = self.page.app.session
        self.busy = True
        self.paint()
        self.page.app.safe_update(self.control)
        try:
            await s.set_sensor_enabled(self.sensor.key, wanted)
            self.page.app.notice(f"{self.sensor.label} {'switched on' if wanted else 'switched off'}.")
        except Exception as error:
            self.page.app.notice(self.page.app.explain(error))
        finally:
            self.busy = False
            self.paint()
            self.page.app.safe_update(self.control)


class SensorsPage:
    name = "Sensors"

    def __init__(self, app):
        self.app = app
        self.rows: list[SensorRow] = []
        self.banner = ft.Column([])
        self.root = None

    def build(self) -> ft.Control:
        s = self.app.session
        self.rows = [SensorRow(self, sensor) for sensor in s.sensors]
        self.root = ft.Column([
            ft.Row([Theme.title("Sensors"),
                    Theme.caption("Sensors are fixed by the board. Switch one off to stop sampling it.")],
                   wrap=True, spacing=14, vertical_alignment=ft.CrossAxisAlignment.CENTER),
            self.banner,
            ft.Column([row.control for row in self.rows], spacing=10),
        ], spacing=16)
        self.refresh("all")
        return self.root

    def needs_rebuild(self) -> bool:
        return False

    def refresh(self, kind: str) -> list:
        if self.root is None:
            return []
        if kind == "sensors" and [r.sensor.key for r in self.rows] != [x.key for x in self.app.session.sensors]:
            self.app.rebuild()
            return []
        s = self.app.session
        if not s.connected:
            text = "Not connected. Connect over Bluetooth to change sensors."
        elif not s.ble.connected:
            text = "Connected over Wi-Fi, which is read-only. Connect over Bluetooth to change sensors."
        elif not s.state_fresh:
            text = "Controller state is stale. Changes are paused until it reports again."
        else:
            text = ""
        self.banner.controls = [Theme.banner(text)] if text else []
        for row in self.rows:
            row.paint()
        return [self.root]

    def confirm_disable(self, row: SensorRow, wanted: bool) -> None:
        def answer(apply: bool):
            self.app.page.pop_dialog()
            if apply:
                self.app.run(row.apply, wanted)
            else:
                row.paint()
                self.app.safe_update(row.control)

        self.app.page.show_dialog(ft.AlertDialog(
            modal=True, title=ft.Text(f"Switch off {row.sensor.label}?"),
            content=ft.Text(f"{row.sensor.depends_note}. You can switch it back on at any time."),
            actions=[ft.TextButton("Cancel", on_click=lambda e: answer(False)),
                     ft.FilledButton("Switch off", on_click=lambda e: answer(True))],
        ))

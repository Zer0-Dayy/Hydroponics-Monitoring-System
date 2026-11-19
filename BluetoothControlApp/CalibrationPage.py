"""Calibration: on-demand, on-device calibration of the sensors that need it.

Only pH (two buffers), TDS (one reference solution, K-factor) and the light
sensor (one reference lux-meter reading) take a user calibration. The other
sensors are factory-calibrated (BME280, DS18B20) or calibrate themselves
(SGP30 baseline).

Each Capture asks the controller for the sensor's raw input right now (it
never touches the sensor bus itself: it reads the latest filtered sample).
Save sends the captured points with their reference values; the controller
checks the fit, stores it in its flash (it survives reboots and firmware
updates) and applies it from the next sample. Reset goes back to the
firmware's factory default.
"""

import flet as ft

import Theme
from Catalog import format_value
from Meter import unit_label
from Protocol import CALIBRATABLE

# sensor -> how the page explains and labels it
GUIDE = {
    "ph": {
        "title": "pH probe (pH-4502C)", "metric": "ph", "icon": ft.Icons.SCIENCE_ROUNDED,
        "steps": "Rinse the probe, put it in the first buffer, wait about a minute for the reading to settle, then "
                 "Capture. Rinse again and repeat with the second buffer. Use buffers at least 1 pH apart "
                 "(7.00 and 4.00 for acidic nutrient solutions).",
        "references": ("Buffer A (pH)", "Buffer B (pH)"), "defaults": ("7.00", "4.00"), "range": (0.0, 14.0),
    },
    "tds": {
        "title": "TDS probe (SEN0244)", "metric": "tds", "icon": ft.Icons.GRAIN_ROUNDED,
        "steps": "Put the probe and the water temperature probe in a reference solution of known TDS, wait for "
                 "both to settle, enter its value and Capture. The controller compensates to 25 °C, so the water "
                 "temperature sensor must be on and reading.",
        "references": ("Reference solution (ppm)",), "defaults": ("707",), "range": (1.0, 2000.0),
    },
    "light": {
        "title": "Light sensor (DFR0026)", "metric": "light_level", "icon": ft.Icons.WB_SUNNY_ROUNDED,
        "steps": "Place a lux meter right next to the light sensor, under the usual light. Enter the meter's "
                 "reading and Capture at the same moment. After calibration the light level is shown in lux; the "
                 "grow-lamp thresholds are not affected.",
        "references": ("Lux meter reading (lux)",), "defaults": ("",), "range": (0.1, 100000.0),
    },
}


class SensorCalibration:
    """One sensor's card: status, live value, reference fields, capture / save / reset."""

    def __init__(self, page: "CalibrationPage", sensor: str):
        self.page = page
        self.sensor = sensor
        self.guide = GUIDE[sensor]
        spec = CALIBRATABLE[sensor]
        self.points = spec["points"]
        self.x_unit = spec["x_unit"]
        self.status = ft.Container()
        self.live = ft.Text("", size=13, color=Theme.MUTED)
        self.fit = Theme.caption("")
        self.refs = [ft.TextField(label=label, value=default, width=200, dense=True,
                                  input_filter=ft.InputFilter(regex_string=r"^[0-9]*[.,]?[0-9]*$"))
                     for label, default in zip(self.guide["references"], self.guide["defaults"])]
        self.captured: list[float | None] = [None] * self.points
        self.captured_text = [ft.Text("not captured", size=12, color=Theme.MUTED, width=150)
                              for _ in range(self.points)]
        self.capture_buttons = [ft.OutlinedButton("Capture", icon=ft.Icons.CAMERA_ROUNDED,
                                                  on_click=lambda e, i=i: page.app.run(self._capture, i))
                                for i in range(self.points)]
        self.save = ft.FilledButton("Save calibration", icon=ft.Icons.SAVE_ROUNDED,
                                    on_click=lambda e: page.app.run(self._save))
        self.reset = ft.TextButton("Reset to factory default", icon=ft.Icons.RESTART_ALT_ROUNDED,
                                   on_click=lambda e: page.app.run(self._reset))
        self.blocked = Theme.caption("")
        rows = [ft.Row([self.refs[i], self.capture_buttons[i], self.captured_text[i]], spacing=10, wrap=True,
                       vertical_alignment=ft.CrossAxisAlignment.CENTER) for i in range(self.points)]
        self.control = Theme.card([
            ft.Row([ft.Icon(self.guide["icon"], color=ft.Colors.PRIMARY),
                    Theme.title(self.guide["title"], 17), ft.Container(expand=True), self.status],
                   vertical_alignment=ft.CrossAxisAlignment.CENTER),
            ft.Row([self.live, self.fit], spacing=16, wrap=True),
            Theme.caption(self.guide["steps"]),
            *rows,
            self.blocked,
            ft.Row([self.save, self.reset], spacing=10, wrap=True),
        ], width=720)

    # ------------------------------------------------------------ painting

    def paint(self, busy: bool) -> None:
        s = self.page.app.session
        info = s.calibration.get(self.sensor)
        if info is None:
            self.status.content = Theme.chip("Unknown", Theme.NEUTRAL)
            self.fit.value = ""
        elif info["overridden"]:
            self.status.content = Theme.chip(f"Calibrated on device · rev {info['rev']}", Theme.GOOD,
                                             ft.Icons.CHECK_CIRCLE_OUTLINE_ROUNDED)
            self.fit.value = self._fit_text(info)
        else:
            self.status.content = Theme.chip("Factory default", Theme.WARN, ft.Icons.TUNE_ROUNDED)
            self.fit.value = self._fit_text(info) if info["valid"] else "No fit: raw values"
        reading = s.readings.get(self.guide["metric"])
        if reading and reading["value"] is not None:
            self.live.value = (f"Now: {format_value(self.guide['metric'], reading['value'])} "
                               f"{unit_label(reading['unit'])}")
        else:
            self.live.value = "Now: no value" + (f" ({reading['health'].lower()})" if reading else "")
        for i, x in enumerate(self.captured):
            self.captured_text[i].value = f"{x:,.1f} {self.x_unit}" if x is not None else "not captured"
            self.captured_text[i].color = Theme.TEXT if x is not None else Theme.MUTED
        reason = self._blocked_reason()
        self.blocked.value = reason or ""
        self.blocked.visible = bool(reason)
        off = busy or reason is not None
        for control in (*self.refs, *self.capture_buttons):
            control.disabled = off
        self.save.disabled = off or any(x is None for x in self.captured)
        self.reset.disabled = busy or not s.can_command or not (info and info["overridden"])

    def _fit_text(self, info: dict) -> str:
        if self.sensor == "ph":
            return f"{info['slope'] * 1000:+.3f} pH per V · {info['offset']:+.2f} pH offset"
        if self.sensor == "tds":
            return f"K = {info['slope']:.3f}"
        return f"{info['slope']:.3f} lux per mV"

    def _blocked_reason(self) -> str | None:
        s = self.page.app.session
        if not s.can_command:
            return "Calibration needs a current Bluetooth connection."
        if self.sensor not in s.enabled:
            return "This sensor is switched off (Sensors page)."
        if self.sensor in ("ph", "tds") and s.power_mode in ("low_power", "transition_to_low", "degraded"):
            return "Low-power mode: this sensor's 5 V supply is off until external power returns."
        if self.sensor == "tds" and "ds18b20" not in s.enabled:
            return "TDS calibration needs the water temperature sensor (DS18B20) switched on."
        return None

    # ------------------------------------------------------------ actions

    def _reference(self, i: int) -> float | None:
        text = (self.refs[i].value or "").strip().replace(",", ".")
        try:
            value = float(text)
        except ValueError:
            return None
        low, high = self.guide["range"]
        return value if low <= value <= high else None

    async def _capture(self, i: int) -> None:
        async def action():
            self.captured[i] = await self.page.app.session.cal_capture(self.sensor)
        await self.page.run(action, f"Captured {self.guide['references'][i].split(' (')[0].lower()}.")

    async def _save(self) -> None:
        refs = [self._reference(i) for i in range(self.points)]
        if any(r is None for r in refs):
            low, high = self.guide["range"]
            self.page.app.notice(f"Enter reference values between {low:g} and {high:g}.")
            return
        if self.sensor == "ph" and abs(refs[0] - refs[1]) < 1.0:
            self.page.app.notice("Use two buffers at least 1 pH apart.")
            return
        if self.sensor == "ph" and abs(self.captured[0] - self.captured[1]) < 1.0:
            self.page.app.notice("Both captures read the same voltage. Was the probe moved to the second buffer?")
            return
        points = list(zip(self.captured, refs))

        async def action():
            await self.page.app.session.set_calibration(self.sensor, points)
            self.captured = [None] * self.points
        await self.page.run(action, "Calibration saved on the controller and applied.")

    async def _reset(self) -> None:
        async def action():
            await self.page.app.session.reset_calibration(self.sensor)
            self.captured = [None] * self.points
        await self.page.run(action, "Back to the factory calibration.")


class CalibrationPage:
    name = "Calibration"

    def __init__(self, app):
        self.app = app
        self.busy = False
        self.cards = {sensor: SensorCalibration(self, sensor) for sensor in CALIBRATABLE}
        self.root = None

    def build(self) -> ft.Control:
        self.root = ft.Column([
            Theme.title("Calibration"),
            Theme.caption("Calibrate only when needed: after replacing a probe, when readings drift, or "
                          "periodically for pH. Results are stored on the controller."),
            *[card.control for card in self.cards.values()],
            Theme.caption("Not listed: BME280 and DS18B20 are factory-calibrated; the SGP30 learns its own "
                          "baseline; the water-level sensor reports distance and needs no calibration."),
        ], spacing=16)
        self.refresh("all")
        if self.app.session.can_command and not self.app.session.calibration:
            self.app.run(self._load)
        return self.root

    def needs_rebuild(self) -> bool:
        return False

    def refresh(self, kind: str) -> list:
        if self.root is None:
            return []
        for card in self.cards.values():
            card.paint(self.busy)
        return [self.root]

    async def _load(self) -> None:
        try:
            await self.app.session.refresh_calibration()
        except Exception as error:
            self.app.notice(self.app.explain(error, "Could not read the calibration"))

    async def run(self, action, done: str) -> None:
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

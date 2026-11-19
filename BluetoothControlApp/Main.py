"""Hydroponics controller desktop app (Windows and Linux). Run with --demo for a simulator.

Main builds the window shell (status bar, navigation rail, page area) and
forwards Session events to the visible page. Data, links and recording live
in Session.py; each page is its own module.
"""

import argparse
import asyncio
import os
import sys

import flet as ft

import Theme
from CalibrationPage import CalibrationPage
from ConnectionPage import ConnectionPage
from DashboardPage import DashboardPage
from MetricsPage import MetricsPage
from SensorsPage import SensorsPage
from Session import GLOBAL, Session
from Store import Store
from WifiPage import WifiPage

# Navigation order: connect first, then the data, then setup (Calibration last).
PAGES = (
    ("Dashboard", ft.Icons.DASHBOARD_OUTLINED, ft.Icons.DASHBOARD_ROUNDED),
    ("Connection", ft.Icons.BLUETOOTH_SEARCHING_OUTLINED, ft.Icons.BLUETOOTH_SEARCHING_ROUNDED),
    ("Wi-Fi", ft.Icons.WIFI_OUTLINED, ft.Icons.WIFI_ROUNDED),
    ("Metrics", ft.Icons.INSIGHTS_OUTLINED, ft.Icons.INSIGHTS_ROUNDED),
    ("Sensors", ft.Icons.SENSORS_OUTLINED, ft.Icons.SENSORS_ROUNDED),
    ("Calibration", ft.Icons.TUNE_OUTLINED, ft.Icons.TUNE_ROUNDED),
)
THEMES = {"system": ft.ThemeMode.SYSTEM, "light": ft.ThemeMode.LIGHT, "dark": ft.ThemeMode.DARK}
THEME_ICONS = {"system": ft.Icons.BRIGHTNESS_AUTO_ROUNDED, "light": ft.Icons.LIGHT_MODE_ROUNDED,
               "dark": ft.Icons.DARK_MODE_ROUNDED}

ERRORS = {
    "rule_latched": "This sensor has a latched or active alarm. Reset the alarm on the Dashboard before switching "
                    "it off.",
    "rule_active": "The alarm condition is still present. It can be reset once the reading is back to normal.",
    "bad_field": "The controller rejected a value in this request.",
    "bad_request": "The controller could not read the request. Check that app and firmware versions match.",
    "unknown_op": "The controller firmware does not support this action. Update the firmware.",
    "expired": "The request waited too long and was not applied. Try again.",
    "timeout": "The controller did not confirm the change in time. Check the result before retrying.",
    "storage": "The controller could not save the change to flash.",
    "stale": "No fresh reading to capture. Check that the sensor is on, powered (not in low-power mode) and, "
             "for TDS, that the water temperature probe works.",
    "bad_fit": "These points give an implausible calibration. Check the reference values and that the probe "
               "settled in each solution, then capture again.",
}


def asset_path(name: str) -> str:
    """Absolute path of a bundled asset, from source or from a PyInstaller build."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "assets", name)


class ControlApp:
    def __init__(self, page: ft.Page, demo: bool):
        self.page = page
        self.store = Store()
        self.session = Session(self.store, demo, self.notice)
        self.session.subscribe(self._session_event)
        self.pages = {"Dashboard": DashboardPage(self), "Metrics": MetricsPage(self), "Sensors": SensorsPage(self),
                      "Calibration": CalibrationPage(self), "Connection": ConnectionPage(self),
                      "Wi-Fi": WifiPage(self)}
        self.current = "Connection" if not demo and self.session.node is None else "Dashboard"
        self.theme_name = self.store.setting(GLOBAL, "theme", "system")
        self.closed = False

        page.title = "Hydroponics Control" + (" (demo)" if demo else "")
        Theme.apply_theme(page)
        page.theme_mode = THEMES.get(self.theme_name, ft.ThemeMode.SYSTEM)
        page.padding = 0
        window = getattr(page, "window", None)
        if window is not None:
            window.min_width, window.min_height = 760, 560
            icon = asset_path("AppIcon.ico" if sys.platform == "win32" else "AppIcon.png")
            if os.path.exists(icon):
                window.icon = icon
        page.on_close = self._close

        self.status = ft.Row(spacing=8, wrap=True, run_spacing=6)
        self.theme_button = ft.IconButton(icon=THEME_ICONS.get(self.theme_name), tooltip="Theme: system / light / dark",
                                          on_click=self._cycle_theme)
        top_bar = ft.Container(
            content=ft.Row([
                self._logo(),
                ft.Column([ft.Text("Hydroponics Control", size=17, weight=ft.FontWeight.BOLD, color=Theme.TEXT),
                           Theme.caption("Demo simulator" if demo else "Local monitoring over Bluetooth", 11)],
                          spacing=0),
                ft.Container(self.status, expand=True, alignment=ft.Alignment.CENTER_RIGHT),
                self.theme_button,
            ], spacing=12, vertical_alignment=ft.CrossAxisAlignment.CENTER),
            padding=ft.Padding.symmetric(horizontal=18, vertical=10), bgcolor=ft.Colors.SURFACE_CONTAINER,
        )
        self.rail = ft.NavigationRail(
            selected_index=[p[0] for p in PAGES].index(self.current), label_type=ft.NavigationRailLabelType.ALL,
            min_width=88, group_alignment=-0.95, on_change=self._rail_changed, bgcolor=ft.Colors.SURFACE_CONTAINER,
            destinations=[ft.NavigationRailDestination(icon=icon, selected_icon=selected, label=name)
                          for name, icon, selected in PAGES])
        self.body = ft.Column(expand=True, scroll=ft.ScrollMode.AUTO)
        page.add(ft.Column([
            top_bar,
            ft.Row([self.rail, ft.Container(self.body, expand=True, padding=ft.Padding.only(left=24, top=20, right=24, bottom=24))],
                   expand=True, spacing=0, vertical_alignment=ft.CrossAxisAlignment.STRETCH),
        ], expand=True, spacing=0))
        self.rebuild()
        page.run_task(self._ticker)

    # ------------------------------------------------------------ shell

    @staticmethod
    def _logo() -> ft.Control:
        # Bytes, not a path: a local path is not served in --web mode or from a PyInstaller build.
        try:
            with open(asset_path("AppIcon-128.png"), "rb") as file:
                return ft.Image(src=file.read(), width=38, height=38)
        except OSError:
            return ft.Icon(ft.Icons.ECO_ROUNDED, color=ft.Colors.PRIMARY, size=30)

    def navigate(self, name: str) -> None:
        self.current = name
        self.rail.selected_index = [p[0] for p in PAGES].index(name)
        self.rebuild()
        self.safe_update(self.rail)

    def rebuild(self) -> None:
        page = self.pages[self.current]
        page.root = None
        self.body.controls = [page.build()]
        self._paint_status()
        self.safe_update(self.page)
        if not self.closed:
            self.page.run_task(self.body.scroll_to, 0)  # a new page starts at its top

    def refresh_page(self, kind: str = "all") -> None:
        if self.closed:
            return
        changed = self.pages[self.current].refresh(kind)
        self._paint_status()
        self.safe_update(self.status, *changed)

    def _rail_changed(self, event) -> None:
        self.navigate(PAGES[event.control.selected_index][0])

    def _session_event(self, kind: str) -> None:
        if self.closed:
            return
        if self.pages[self.current].needs_rebuild():
            self.rebuild()
            return
        self.refresh_page(kind)

    async def _ticker(self) -> None:
        """Ages ("12 s ago"), state staleness and link badges move without new frames."""
        while not self.closed:
            await asyncio.sleep(2)
            # Hardware diagnostics: one small request per 10 s, only while someone looks at them.
            if self.current == "Dashboard" and self.session.diagnostics_due:
                self.run(self.session.refresh_diagnostics)
            if self.current in ("Dashboard", "Sensors", "Calibration", "Connection", "Wi-Fi"):
                self.refresh_page("tick")
            else:
                self._paint_status()
                self.safe_update(self.status)

    def _paint_status(self) -> None:
        s = self.session
        state = s.state or {}
        chips = []
        if s.node:
            chips.append(Theme.chip(s.node, Theme.SEED, ft.Icons.MEMORY_ROUNDED))
        if s.connected:
            chips.append(Theme.chip("Bluetooth", Theme.GOOD, ft.Icons.BLUETOOTH_CONNECTED_ROUNDED))
        elif s.ble_reconnecting:
            chips.append(Theme.chip("Reconnecting…", Theme.WARN, ft.Icons.SYNC_ROUNDED))
        else:
            chips.append(Theme.chip("Offline", Theme.NEUTRAL, ft.Icons.LINK_OFF_ROUNDED))
        if s.connected and not s.state_fresh:
            chips.append(Theme.chip("State stale", Theme.BAD, ft.Icons.HOURGLASS_BOTTOM_ROUNDED))
        if state:
            label, color, icon = Theme.power_style(s.power_mode)
            chips.append(Theme.chip(label, color, icon))
            held = set(state["safety"]["latched"]) | set(state["safety"]["active"])
            if held:
                chips.append(Theme.chip(f"{len(held)} alarm{'s' if len(held) > 1 else ''}", Theme.BAD,
                                        ft.Icons.WARNING_AMBER_ROUNDED))
        self.status.controls = chips

    def _cycle_theme(self, _event) -> None:
        order = list(THEMES)
        self.theme_name = order[(order.index(self.theme_name) + 1) % len(order)] if self.theme_name in order \
            else "system"
        self.store.set_setting(GLOBAL, "theme", self.theme_name)
        self.page.theme_mode = THEMES[self.theme_name]
        self.theme_button.icon = THEME_ICONS[self.theme_name]
        self.safe_update(self.page)

    # ------------------------------------------------------------ helpers for pages

    def run(self, handler, *args) -> None:
        self.page.run_task(handler, *args)

    def safe_update(self, *controls) -> None:
        if self.closed:
            return
        try:
            if len(controls) == 1 and controls[0] is self.page:
                self.page.update()
            else:
                for control in controls:
                    if control is not None and getattr(control, "page", None) is not None:
                        control.update()
        except (RuntimeError, AssertionError):
            pass  # the control was replaced by a rebuild in the meantime

    def notice(self, message: str) -> None:
        if not self.closed:
            self.page.show_dialog(ft.SnackBar(ft.Text(message), duration=ft.Duration(seconds=5)))

    @staticmethod
    def explain(error: Exception, prefix: str = "") -> str:
        text = ERRORS.get(str(error), str(error) or error.__class__.__name__)
        return f"{prefix}: {text}" if prefix else text

    async def _close(self, _event) -> None:
        self.closed = True
        await self.session.close()
        self.store.close()


def arguments():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--web", type=int, metavar="PORT", help="serve in a browser instead of a window (dev)")
    parser.add_argument("--log", metavar="FILE", help="write link decisions (Bluetooth) to FILE")
    return parser.parse_known_args()[0]


def main(page: ft.Page):
    log_file = arguments().log
    if log_file:
        import logging
        handler = logging.FileHandler(log_file, encoding="utf-8")
        handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
        logger = logging.getLogger("session")
        logger.setLevel(logging.DEBUG)
        logger.addHandler(handler)
    ControlApp(page, arguments().demo)


if __name__ == "__main__":
    port = arguments().web
    if port:
        import logging
        logging.basicConfig(level=logging.WARNING)
        os.environ["FLET_FORCE_WEB_SERVER"] = "true"  # serve only; open http://localhost:PORT yourself
        ft.run(main, view=ft.AppView.WEB_BROWSER, port=port)
    else:
        ft.run(main)

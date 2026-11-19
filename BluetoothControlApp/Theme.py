"""Shared look: theme colours, cards, chips and health styling.

Surfaces and text use the Material colour roles, so light and dark themes both
work. Status colours are fixed hues shown on a tinted background of the same
hue, which reads in either theme.
"""

import flet as ft

SEED = "#087F78"
GOOD = "#1E8A63"
WARN = "#B7791F"
BAD = "#C2493D"
INFO = "#2B6CB0"
NEUTRAL = "#6B7C83"

TEXT = ft.Colors.ON_SURFACE
MUTED = ft.Colors.ON_SURFACE_VARIANT
CARD_BG = ft.Colors.SURFACE_CONTAINER_LOW
PAGE_BG = ft.Colors.SURFACE
OUTLINE = ft.Colors.OUTLINE_VARIANT

# health -> (label, colour)
HEALTH_STYLE = {
    "OK": ("Live", GOOD),
    "UNCALIBRATED": ("Uncalibrated", WARN),
    "STARTING": ("Starting", NEUTRAL),
    "WARMING": ("Warming up", INFO),
    "STALE": ("Stale", NEUTRAL),
    "FAULT": ("Fault", BAD),
    "TIMEOUT": ("Timeout", BAD),
    "UNPOWERED": ("Unpowered", NEUTRAL),
    "DEPENDENCY_INVALID": ("Dependency missing", WARN),
    "DISABLED": ("Disabled", NEUTRAL),
    "NO_DATA": ("No data", NEUTRAL),
}
BAND_COLOR = {"Low": WARN, "Normal": GOOD, "High": BAD}
# Health chips hidden on the Dashboard cards: normal operation needs no label.
QUIET_HEALTH = {"OK"}  # UNCALIBRATED is shown: its numbers are not trustworthy units

# power mode (State "mode", WP9) -> (label, colour, icon)
POWER_STYLE = {
    "normal": ("Normal power", GOOD, ft.Icons.POWER_ROUNDED),
    "transition_to_low": ("Switching to low power…", INFO, ft.Icons.SYNC_ROUNDED),
    "low_power": ("Low-power mode", WARN, ft.Icons.BATTERY_SAVER_ROUNDED),
    "transition_to_normal": ("Switching to normal power…", INFO, ft.Icons.SYNC_ROUNDED),
    "degraded": ("Power transition degraded", BAD, ft.Icons.ERROR_OUTLINE_ROUNDED),
    "unknown": ("Power mode not reported", NEUTRAL, ft.Icons.BOLT_ROUNDED),
}


def power_style(mode: str) -> tuple:
    return POWER_STYLE.get(mode, POWER_STYLE["unknown"])


def apply_theme(page: ft.Page) -> None:
    page.theme = ft.Theme(color_scheme_seed=SEED)
    page.dark_theme = ft.Theme(color_scheme_seed=SEED)
    page.bgcolor = PAGE_BG


def tint(color: str, opacity: float = 0.14) -> str:
    return ft.Colors.with_opacity(opacity, color)


def chip(label: str, color: str, icon: str | None = None, size: int = 12) -> ft.Container:
    content = ft.Text(label, size=size, weight=ft.FontWeight.W_600, color=color, no_wrap=True)
    if icon:
        content = ft.Row([ft.Icon(icon, size=size + 3, color=color), content], spacing=4, tight=True)
    return ft.Container(content=content, bgcolor=tint(color), border_radius=20,
                        padding=ft.Padding.symmetric(horizontal=10, vertical=5))


def health_chip(health: str) -> ft.Container:
    label, color = HEALTH_STYLE.get(health, (health.title(), NEUTRAL))
    return chip(label, color)


def card(controls: list, width: float | None = None, padding: int = 18, expand=None,
         spacing: int = 10) -> ft.Container:
    return ft.Container(content=ft.Column(controls, spacing=spacing, tight=True), bgcolor=CARD_BG,
                        border_radius=16, padding=padding, width=width, expand=expand,
                        border=ft.Border.all(1, OUTLINE))


def title(text: str, size: int = 20) -> ft.Text:
    return ft.Text(text, size=size, weight=ft.FontWeight.BOLD, color=TEXT)


def caption(text: str, size: int = 12) -> ft.Text:
    return ft.Text(text, size=size, color=MUTED)


def overline(text: str) -> ft.Text:
    return ft.Text(text.upper(), size=11, weight=ft.FontWeight.W_600, color=MUTED)


def banner(text: str, color: str = WARN, icon: str = ft.Icons.INFO_OUTLINE_ROUNDED,
           action: ft.Control | None = None) -> ft.Container:
    row = [ft.Icon(icon, color=color, size=20), ft.Text(text, color=TEXT, expand=True, size=13)]
    if action is not None:
        row.append(action)
    return ft.Container(content=ft.Row(row, spacing=10), bgcolor=tint(color, 0.12), border_radius=12,
                        padding=ft.Padding.symmetric(horizontal=14, vertical=10),
                        border=ft.Border.all(1, tint(color, 0.35)))


def empty_state(icon: str, heading: str, text: str, actions: list | None = None) -> ft.Container:
    return ft.Container(
        content=ft.Column([
            ft.Icon(icon, size=44, color=ft.Colors.PRIMARY),
            title(heading, 20),
            ft.Text(text, color=MUTED, text_align=ft.TextAlign.CENTER, width=420),
            ft.Row(actions or [], alignment=ft.MainAxisAlignment.CENTER, spacing=10),
        ], horizontal_alignment=ft.CrossAxisAlignment.CENTER, spacing=12, tight=True),
        padding=40, alignment=ft.Alignment.CENTER,
    )


def ago(seconds: float | None) -> str:
    if seconds is None:
        return "never measured"
    if seconds < 2:
        return "just now"
    if seconds < 90:
        return f"{seconds:.0f} s ago"
    if seconds < 5400:
        return f"{seconds / 60:.0f} min ago"
    return f"{seconds / 3600:.1f} h ago"

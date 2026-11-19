"""Small canvas charts: a sparkline for metric cards and a time-series chart.

Flet 1.0 moved its chart controls to a separate package, so these are drawn
directly on flet.canvas, which keeps the app at one UI dependency.
"""

from datetime import datetime

import flet as ft
import flet.canvas as cv

import Theme


def _stroke(color: str, width: float = 2.0) -> ft.Paint:
    return ft.Paint(color=color, stroke_width=width, style=ft.PaintingStyle.STROKE,
                    stroke_cap=ft.StrokeCap.ROUND, stroke_join=ft.StrokeJoin.ROUND, anti_alias=True)


def _path(points, x0, y0, w, h, t_lo, t_hi, v_lo, v_hi) -> list:
    t_span = (t_hi - t_lo) or 1
    v_span = (v_hi - v_lo) or 1
    elements = []
    for i, (t, v) in enumerate(points):
        x = x0 + (t - t_lo) / t_span * w
        y = y0 + h - (v - v_lo) / v_span * h
        elements.append(cv.Path.MoveTo(x, y) if i == 0 else cv.Path.LineTo(x, y))
    return elements


class Sparkline:
    """Owns a fixed-size canvas (self.control)."""

    def __init__(self, width: float = 120, height: float = 34, color: str = Theme.SEED):
        self.width, self.height, self.color = width, height, color
        self.control = cv.Canvas(width=width, height=height, shapes=[])

    def set_points(self, points: list[tuple[int, float]], color: str | None = None) -> None:
        self.color = color or self.color
        if len(points) < 2:
            self.control.shapes = []
            return
        values = [v for _, v in points]
        lo, hi = min(values), max(values)
        pad = (hi - lo) * 0.1 or 1
        self.control.shapes = [cv.Path(_path(points, 2, 2, self.width - 4, self.height - 4, points[0][0], points[-1][0],
                                     lo - pad, hi + pad), paint=_stroke(self.color, 1.8))]


class TimeChart:
    """One series with min / max and start / end labels; self.control resizes with its parent."""

    PAD_LEFT, PAD_RIGHT, PAD_TOP, PAD_BOTTOM = 56, 12, 12, 26

    def __init__(self, height: float = 260):
        self.canvas = cv.Canvas(expand=True, on_resize=self._resized, resize_interval=150)
        self.control = ft.Container(content=self.canvas, height=height, expand=True)
        self.points: list[tuple[int, float]] = []
        self.unit = ""
        self.size = (700.0, height)
        self.empty_text = "No usable values in this range"

    def _resized(self, event) -> None:
        self.size = (event.width, event.height)
        self._draw()
        self.canvas.update()

    def set_series(self, points: list[tuple[int, float]], unit: str, empty_text: str | None = None) -> None:
        self.points, self.unit = points, unit
        if empty_text:
            self.empty_text = empty_text
        self._draw()

    def _label(self, x, y, text, align=ft.Alignment.CENTER_LEFT) -> cv.Text:
        return cv.Text(x, y, text, style=ft.TextStyle(size=11, color=Theme.MUTED), alignment=align)

    def _draw(self) -> None:
        width, height = self.size
        x0, y0 = self.PAD_LEFT, self.PAD_TOP
        w = max(10.0, width - self.PAD_LEFT - self.PAD_RIGHT)
        h = max(10.0, height - self.PAD_TOP - self.PAD_BOTTOM)
        grid = ft.Paint(color=Theme.OUTLINE, stroke_width=1, style=ft.PaintingStyle.STROKE)
        shapes = [cv.Line(x0, y0 + h * f, x0 + w, y0 + h * f, paint=grid) for f in (0, 0.5, 1)]
        if len(self.points) < 2:
            shapes.append(self._label(x0 + w / 2, y0 + h / 2, self.empty_text, ft.Alignment.CENTER))
            self.canvas.shapes = shapes
            return
        values = [v for _, v in self.points]
        lo, hi = min(values), max(values)
        pad = (hi - lo) * 0.08 or max(abs(hi) * 0.05, 1)
        v_lo, v_hi = lo - pad, hi + pad
        t_lo, t_hi = self.points[0][0], self.points[-1][0]
        shapes.append(cv.Path(_path(self.points, x0, y0, w, h, t_lo, t_hi, v_lo, v_hi),
                              paint=_stroke(Theme.SEED, 2.2)))
        for fraction, value in ((0, v_hi), (0.5, (v_lo + v_hi) / 2), (1, v_lo)):
            shapes.append(self._label(x0 - 8, y0 + h * fraction, f"{value:,.4g}", ft.Alignment.CENTER_RIGHT))
        shapes.append(self._label(x0, y0 + h + 14, _clock(t_lo, t_hi - t_lo), ft.Alignment.CENTER_LEFT))
        shapes.append(self._label(x0 + w, y0 + h + 14, _clock(t_hi, t_hi - t_lo), ft.Alignment.CENTER_RIGHT))
        if self.unit:
            shapes.append(self._label(4, y0, self.unit, ft.Alignment.CENTER_LEFT))
        self.canvas.shapes = shapes


def _clock(ms: int, span_ms: int) -> str:
    moment = datetime.fromtimestamp(ms / 1000)
    return moment.strftime("%H:%M:%S" if span_ms < 6 * 3600_000 else "%d %b %H:%M")

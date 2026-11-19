"""Metrics: recorded history per controller, summary, chart and CSV/JSON download.

Works offline: everything comes from the local store, so history recorded
earlier stays available without a connection.
"""

import asyncio
import re
from datetime import datetime
from pathlib import Path

import flet as ft

import Theme
from Catalog import METRICS, format_value, metric_label
from Charts import TimeChart
from Export import export_meta, write_csv, write_json
from Meter import unit_label
from Store import now_ms

RANGES = (("1h", "1 h", 3600_000), ("24h", "24 h", 86400_000), ("7d", "7 days", 7 * 86400_000),
          ("all", "All", None), ("custom", "Custom", None))
CUSTOM_FORMAT = "%Y-%m-%d %H:%M"


def kind_has_nodes(app) -> bool:
    return app.session.node is not None and app.session.recorded > 0


class MetricsPage:
    name = "Metrics"

    def __init__(self, app):
        self.app = app
        self.range = "24h"
        self.selected = {info.key for info in METRICS}
        self.chart_metric = "air_temperature"
        self.healthy_only = False
        self.node_dropdown = ft.Dropdown(label="Controller", width=260, on_select=self._node_changed)
        self.range_buttons = ft.SegmentedButton(
            segments=[ft.Segment(value=key, label=ft.Text(label)) for key, label, _ in RANGES],
            selected=[self.range], on_change=self._range_changed, show_selected_icon=False)
        self.custom_from = ft.TextField(label="From (local)", hint_text="2026-10-25 08:00", width=190, dense=True)
        self.custom_to = ft.TextField(label="To (local)", hint_text="blank = now", width=190, dense=True)
        self.custom_row = ft.Row([self.custom_from, self.custom_to,
                                  ft.OutlinedButton("Apply", on_click=lambda e: self.reload())],
                                 visible=False, spacing=10)
        self.metric_chips = ft.Row(wrap=True, spacing=6, run_spacing=6)
        self.healthy_box = ft.Checkbox(label="Export usable values only", value=False, on_change=self._healthy_changed)
        self.table = ft.DataTable(columns=[ft.DataColumn(ft.Text(name), numeric=num) for name, num in (
            ("Metric", False), ("Latest", True), ("Min", True), ("Max", True), ("Mean", True),
            ("Samples", True), ("Usable", True))], column_spacing=28, heading_row_height=40, data_row_min_height=40)
        self.empty = Theme.caption("")
        self.chart = TimeChart(260)
        self.chart_select = ft.Dropdown(label="Chart", width=260, on_select=self._chart_changed,
                                        options=[ft.DropdownOption(key=i.key, text=i.label) for i in METRICS],
                                        value=self.chart_metric)
        self.export_status = Theme.caption("")
        self.picker = None
        self.root = None

    # ------------------------------------------------------------ layout

    def build(self) -> ft.Control:
        nodes = self.app.session.store.nodes()
        node = self._node(nodes)
        self.node_dropdown.options = [ft.DropdownOption(key=n, text=n) for n in nodes]
        self.node_dropdown.value = node
        self._build_chips()
        if not nodes:
            self.root = Theme.empty_state(
                ft.Icons.INSIGHTS_ROUNDED, "No recorded metrics yet",
                "Readings are recorded on this computer while it is connected to a controller, on any page "
                "and over Bluetooth or Wi-Fi.",
                [ft.FilledButton("Connect a controller", icon=ft.Icons.SEARCH_ROUNDED,
                                 on_click=lambda e: self.app.navigate("Connection"))])
            return self.root
        self.root = ft.Column([
            ft.Row([Theme.title("Metrics"), Theme.caption(
                "Recorded on this computer. Resolution about 3 s (the telemetry period); times shown in local time, "
                "exported in UTC.")], wrap=True, spacing=14, vertical_alignment=ft.CrossAxisAlignment.CENTER),
            Theme.card([
                ft.Row([self.node_dropdown, self.range_buttons], wrap=True, spacing=14, run_spacing=10,
                       vertical_alignment=ft.CrossAxisAlignment.CENTER),
                self.custom_row,
                Theme.overline("Metrics"),
                self.metric_chips,
                ft.Row([self.healthy_box,
                        ft.Row([ft.OutlinedButton("Refresh", icon=ft.Icons.REFRESH_ROUNDED,
                                                  on_click=lambda e: self.reload()),
                                ft.FilledButton("Download CSV", icon=ft.Icons.TABLE_VIEW_ROUNDED,
                                                on_click=lambda e: self.app.run(self._export, "csv")),
                                ft.FilledButton("Download JSON", icon=ft.Icons.DATA_OBJECT_ROUNDED,
                                                on_click=lambda e: self.app.run(self._export, "json"))],
                               spacing=8, wrap=True)],
                       alignment=ft.MainAxisAlignment.SPACE_BETWEEN, wrap=True),
                self.export_status,
            ], spacing=12),
            Theme.card([ft.Row([Theme.title("Trend", 17), self.chart_select],
                               alignment=ft.MainAxisAlignment.SPACE_BETWEEN), self.chart.control]),
            Theme.card([Theme.title("Summary", 17), self.empty,
                        ft.Row([self.table], scroll=ft.ScrollMode.AUTO)]),
        ], spacing=16)
        self.reload(update=False)
        return self.root

    def _build_chips(self) -> None:
        def toggle(key):
            self.selected.symmetric_difference_update({key})
            self._build_chips()
            self.reload()
        self.metric_chips.controls = [
            ft.Chip(label=ft.Text(info.label, size=12), selected=info.key in self.selected,
                    show_checkmark=True, on_select=lambda e, k=info.key: toggle(k))
            for info in METRICS
        ] + [ft.TextButton("All", on_click=lambda e: self._select_all(True)),
             ft.TextButton("None", on_click=lambda e: self._select_all(False))]

    def _select_all(self, value: bool) -> None:
        self.selected = {i.key for i in METRICS} if value else set()
        self._build_chips()
        self.reload()

    def _node(self, nodes: list[str]) -> str | None:
        current = self.node_dropdown.value
        if current in nodes:
            return current
        if self.app.session.node in nodes:
            return self.app.session.node
        return nodes[0] if nodes else None

    # ------------------------------------------------------------ data

    def _window(self) -> tuple[int | None, int | None] | None:
        if self.range == "custom":
            try:
                since = self._parse_local(self.custom_from.value)
                until = self._parse_local(self.custom_to.value) if (self.custom_to.value or "").strip() else None
            except ValueError:
                return None
            return since, until
        span = next(span for key, _, span in RANGES if key == self.range)
        return (now_ms() - span if span else None), None

    @staticmethod
    def _parse_local(text: str | None) -> int:
        text = re.sub(r"\s+", " ", (text or "").strip())
        return int(datetime.strptime(text, CUSTOM_FORMAT).timestamp() * 1000)

    def reload(self, update: bool = True) -> None:
        if self.root is None or not self.node_dropdown.value:
            return
        window = self._window()
        node = self.node_dropdown.value
        if window is None:
            self.empty.value = f"Enter dates as {datetime.now().strftime(CUSTOM_FORMAT)}."
            self.table.rows = []
        else:
            since, until = window
            metrics = sorted(self.selected)
            summary = self.app.session.store.summary(node, metrics, since, until) if metrics else {}
            rows = []
            for info in METRICS:
                item = summary.get(info.key)
                if item is None:
                    continue
                unit = unit_label(item["unit"] or "")
                usable = f"{100 * item['usable'] / item['count']:.0f} %" if item["count"] else "—"
                if item["other_units"]:  # calibrated in this range: older values are in another unit
                    older = ", ".join(unit_label(u) for u in item["other_units"])
                    unit = f"{unit}; {item['excluded']:,} earlier {older} values not included"

                def num(value, key=info.key):
                    return format_value(key, value) if value is not None else "—"
                rows.append(ft.DataRow(cells=[
                    ft.DataCell(ft.Text(f"{info.label} ({unit})" if unit else info.label)),
                    ft.DataCell(ft.Text(num(item["latest"]))), ft.DataCell(ft.Text(num(item["min"]))),
                    ft.DataCell(ft.Text(num(item["max"]))), ft.DataCell(ft.Text(num(item["mean"]))),
                    ft.DataCell(ft.Text(f"{item['count']:,}")), ft.DataCell(ft.Text(usable)),
                ]))
            self.table.rows = rows
            self.empty.value = "" if rows else "No samples for this selection."
            chart_unit = summary.get(self.chart_metric, {}).get("unit")
            series = self.app.session.store.series(node, self.chart_metric, since, until, 500, chart_unit)
            unit = unit_label(chart_unit or "")
            self.chart.set_series(series, unit, f"No usable {metric_label(self.chart_metric).lower()} values "
                                                f"in this range")
        if update:
            self.app.safe_update(self.root)

    def needs_rebuild(self) -> bool:
        # The empty state turns into the full page once the first samples arrive.
        return self.root is not None and not self.node_dropdown.options and kind_has_nodes(self.app)

    def refresh(self, kind: str) -> list:
        return []  # history is reloaded on demand, not on every frame

    # ------------------------------------------------------------ events

    def _node_changed(self, _event) -> None:
        self.reload()

    def _range_changed(self, event) -> None:
        selected = list(event.control.selected or [])
        self.range = selected[0] if selected else "24h"
        self.custom_row.visible = self.range == "custom"
        self.reload()

    def _healthy_changed(self, event) -> None:
        self.healthy_only = bool(event.control.value)

    def _chart_changed(self, event) -> None:
        self.chart_metric = event.control.value or self.chart_metric
        self.reload()

    async def _export(self, kind: str) -> None:
        node = self.node_dropdown.value
        window = self._window()
        if not node or window is None:
            self.app.notice("Choose a controller and a valid time range first.")
            return
        if not self.selected:
            self.app.notice("Select at least one metric to export.")
            return
        if self.picker is None:
            self.picker = ft.FilePicker()
            self.app.page.services.append(self.picker)
            self.app.page.update()
        stamp = datetime.now().strftime("%Y%m%d-%H%M")
        safe_node = re.sub(r"[^A-Za-z0-9_-]+", "-", node).strip("-") or "controller"
        path = await self.picker.save_file(
            dialog_title=f"Save metrics as {kind.upper()}", file_name=f"{safe_node}-metrics-{stamp}.{kind}",
            allowed_extensions=[kind], file_type=ft.FilePickerFileType.CUSTOM)
        if not path:
            return
        path = Path(path)
        if path.suffix.lower() != f".{kind}":
            path = path.with_suffix(f".{kind}")
        since, until = window
        metrics = sorted(self.selected)
        store = self.app.session.store
        self.export_status.value = "Exporting…"
        self.app.safe_update(self.export_status)
        state = self.app.session.state or {}
        fw = state.get("fw", "") if self.app.session.node == node else ""
        try:
            count = await asyncio.to_thread(_export_job, store.path.parent, kind, path, node, metrics, since,
                                            until, self.healthy_only, fw)
            self.export_status.value = f"Saved {count:,} samples to {path}"
            self.app.notice(f"Exported {count:,} samples.")
        except OSError as error:
            self.export_status.value = f"Export failed: {error}"
        self.app.safe_update(self.export_status)


def _export_job(directory, kind, path, node, metrics, since, until, healthy_only, fw) -> int:
    """Runs in a worker thread with its own sqlite connection."""
    from Store import Store
    reader = Store(directory)
    try:
        rows = reader.rows(node, metrics, since, until, healthy_only)
        if kind == "csv":
            return write_csv(path, rows)
        meta = export_meta(node, metrics, since, until, healthy_only, reader.boots(node, since, until),
                           reader.config_revisions(node, since, until), fw)
        return write_json(path, meta, rows)
    finally:
        reader.close()

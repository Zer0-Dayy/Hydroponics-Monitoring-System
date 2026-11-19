"""CSV and JSON export of recorded samples (Metrics page).

Rows are streamed from the store, so large histories never sit in memory.
Timestamps are ISO 8601 UTC. Values are empty (CSV) or null (JSON) when the
controller reported no usable value; the health column says why.
"""

import csv
import json
from datetime import datetime, timezone
from pathlib import Path

COLUMNS = ("sample_utc", "received_utc", "node", "boot", "metric", "seq", "value", "unit",
           "health", "quality", "cfg_rev", "cal_rev", "link")


def iso(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _record(row: tuple) -> dict:
    sample_ms, received_ms, node, boot, metric, seq, value, unit, health, quality, cfg_rev, cal_rev, link = row
    return {"sample_utc": iso(sample_ms), "received_utc": iso(received_ms), "node": node, "boot": boot,
            "metric": metric, "seq": seq, "value": value, "unit": unit, "health": health, "quality": quality,
            "cfg_rev": cfg_rev, "cal_rev": cal_rev, "link": link}


def write_csv(path: Path, rows) -> int:
    count = 0
    with open(path, "w", newline="", encoding="utf-8-sig") as handle:  # BOM so Excel reads UTF-8
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        for row in rows:
            record = _record(row)
            if record["value"] is None:
                record["value"] = ""
            writer.writerow(record)
            count += 1
    return count


def write_json(path: Path, meta: dict, rows) -> int:
    """{"meta": {...}, "samples": [...]}, written incrementally."""
    count = 0
    with open(path, "w", encoding="utf-8") as handle:
        handle.write('{"meta":')
        json.dump(meta, handle, ensure_ascii=False)
        handle.write(',"samples":[')
        for row in rows:
            if count:
                handle.write(",")
            handle.write("\n")
            json.dump(_record(row), handle, ensure_ascii=False, separators=(",", ":"))
            count += 1
        handle.write("\n]}\n")
    return count


def export_meta(node: str, metrics: list[str], since_ms: int | None, until_ms: int | None,
                healthy_only: bool, boots: list[int], cfg_revs: list[int], fw: str = "") -> dict:
    return {
        "exported_utc": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "app": "Hydroponics Control",
        "contract_version": 2,
        "node": node,
        "firmware": fw,
        "boots": boots,
        "config_revisions": cfg_revs,
        "filters": {
            "metrics": metrics,
            "since_utc": iso(since_ms) if since_ms is not None else None,
            "until_utc": iso(until_ms) if until_ms is not None else None,
            "usable_values_only": healthy_only,
        },
        "notes": "sample_utc = laptop receive time minus the controller's reported age. "
                 "History resolution is the 3 s telemetry period.",
    }

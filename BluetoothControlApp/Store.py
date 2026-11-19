"""Laptop-side history of received samples. WiFi passwords are never stored here.

The ESP has no wall clock and keeps no history: the app records every
telemetry frame it receives, on any page and over either link. Sample time is
the receive time minus the reported age. The firmware re-sends each 1 s
sample in its 3 s telemetry frame and both links can deliver the same frame,
so samples are stored once per (node, boot, metric, seq); history resolution
is therefore about 3 s.

The v1 `Readings` table of earlier versions is left untouched.
"""

import sqlite3
import sys
import time
from pathlib import Path


def default_directory() -> Path:
    from platformdirs import user_data_dir
    return Path(user_data_dir("HydroponicsControlApp", appauthor=False))


def now_ms() -> int:
    return int(time.time() * 1000)


class Store:
    def __init__(self, directory: Path | None = None):
        directory = directory or default_directory()
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        database = directory / "Readings.sqlite3"
        self.path = database
        self.connection = sqlite3.connect(database)
        if sys.platform != "win32":
            database.chmod(0o600)
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS Samples (
                Id INTEGER PRIMARY KEY, Node TEXT NOT NULL, Boot INTEGER NOT NULL,
                Metric TEXT NOT NULL, Seq INTEGER NOT NULL, SampleMs INTEGER NOT NULL,
                ReceivedMs INTEGER NOT NULL, Value REAL, Unit TEXT NOT NULL, Health TEXT NOT NULL,
                Quality INTEGER NOT NULL, CfgRev INTEGER NOT NULL, CalRev INTEGER NOT NULL,
                Link TEXT NOT NULL, UNIQUE(Node, Boot, Metric, Seq));
            CREATE INDEX IF NOT EXISTS SamplesByTime ON Samples(Node, Metric, SampleMs);
            CREATE TABLE IF NOT EXISTS Events (
                Id INTEGER PRIMARY KEY, Node TEXT NOT NULL, Boot INTEGER NOT NULL, N INTEGER NOT NULL,
                Rule INTEGER NOT NULL, Level TEXT NOT NULL, Kind TEXT NOT NULL, EventMs INTEGER NOT NULL,
                Gap INTEGER NOT NULL, UNIQUE(Node, Boot, N));
            CREATE TABLE IF NOT EXISTS Settings (
                Node TEXT NOT NULL, Key TEXT NOT NULL, Value TEXT NOT NULL, PRIMARY KEY(Node, Key));
        """)
        self.connection.commit()

    # ---------------------------------------------------------------- writes

    def record_samples(self, node: str, boot: int, cfg_rev: int, samples: list[dict], link: str,
                       received_ms: int | None = None) -> int:
        """Stores new samples; returns how many were not already recorded."""
        received_ms = received_ms or now_ms()
        rows = []
        for s in samples:
            if s["age_ms"] is None:
                continue  # never measured since boot: nothing to record
            rows.append((node, boot, s["metric"], s["seq"], received_ms - s["age_ms"], received_ms,
                         s["value"], s["unit"], s["health"], s["quality"], cfg_rev, s["cal_rev"], link))
        before = self.connection.total_changes
        self.connection.executemany(
            "INSERT OR IGNORE INTO Samples(Node, Boot, Metric, Seq, SampleMs, ReceivedMs, Value, Unit, "
            "Health, Quality, CfgRev, CalRev, Link) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
        self.connection.commit()
        return self.connection.total_changes - before

    def record_event(self, node: str, boot: int, event: dict, received_ms: int | None = None) -> bool:
        received_ms = received_ms or now_ms()
        cursor = self.connection.execute(
            "INSERT OR IGNORE INTO Events(Node, Boot, N, Rule, Level, Kind, EventMs, Gap) VALUES(?,?,?,?,?,?,?,?)",
            (node, boot, event["n"], event["rule"], event["level"], event["kind"],
             received_ms - event["age_ms"], int(event["gap"])))
        self.connection.commit()
        return cursor.rowcount > 0

    def set_setting(self, node: str, key: str, value: str) -> None:
        self.connection.execute(
            "INSERT INTO Settings(Node, Key, Value) VALUES(?,?,?) "
            "ON CONFLICT(Node, Key) DO UPDATE SET Value = excluded.Value", (node, key, value))
        self.connection.commit()

    def setting(self, node: str, key: str, default: str | None = None) -> str | None:
        row = self.connection.execute("SELECT Value FROM Settings WHERE Node = ? AND Key = ?",
                                      (node, key)).fetchone()
        return row[0] if row else default

    def delete_settings(self, node: str, *keys: str) -> None:
        for key in keys:
            self.connection.execute("DELETE FROM Settings WHERE Node = ? AND Key = ?", (node, key))
        self.connection.commit()

    # ---------------------------------------------------------------- reads

    def nodes(self) -> list[str]:
        return [row[0] for row in self.connection.execute("SELECT DISTINCT Node FROM Samples ORDER BY Node")]

    def count(self, node: str | None = None) -> int:
        if node is None:
            return self.connection.execute("SELECT COUNT(*) FROM Samples").fetchone()[0]
        return self.connection.execute("SELECT COUNT(*) FROM Samples WHERE Node = ?", (node,)).fetchone()[0]

    @staticmethod
    def _where(node, metrics, since_ms, until_ms, healthy_only, unit=None):
        clauses, args = ["Node = ?"], [node]
        if unit is not None:
            clauses.append("Unit = ?")
            args.append(unit)
        if metrics:
            clauses.append(f"Metric IN ({','.join('?' * len(metrics))})")
            args.extend(metrics)
        if since_ms is not None:
            clauses.append("SampleMs >= ?")
            args.append(since_ms)
        if until_ms is not None:
            clauses.append("SampleMs <= ?")
            args.append(until_ms)
        if healthy_only:
            clauses.append("Value IS NOT NULL")
        return " AND ".join(clauses), args

    def latest_unit(self, node: str, metric: str, since_ms: int | None = None,
                    until_ms: int | None = None) -> str | None:
        """Unit of the newest usable sample in the window (a calibration can change it, e.g. mV -> lux)."""
        where, args = self._where(node, [metric], since_ms, until_ms, True)
        row = self.connection.execute(
            f"SELECT Unit FROM Samples WHERE {where} ORDER BY SampleMs DESC LIMIT 1", args).fetchone()
        return row[0] if row else None

    def summary(self, node: str, metrics: list[str] | None = None, since_ms: int | None = None,
                until_ms: int | None = None) -> dict[str, dict]:
        """Per metric: count, usable count, then min, max, mean and the latest usable value.

        Values in different units are never mixed: min / max / mean / latest
        use only the metric's latest unit; "other_units" lists the units of
        the samples left out (recorded before a calibration changed it), and
        "excluded" counts those usable samples.
        """
        where, args = self._where(node, metrics, since_ms, until_ms, False)
        out = {}
        for metric, count, usable in self.connection.execute(
                f"SELECT Metric, COUNT(*), COUNT(Value) FROM Samples WHERE {where} GROUP BY Metric", args):
            out[metric] = {"count": count, "usable": usable, "min": None, "max": None, "mean": None,
                           "unit": None, "latest": None, "latest_ms": None, "other_units": [], "excluded": 0}
        for metric, item in out.items():
            unit = self.latest_unit(node, metric, since_ms, until_ms)
            if unit is None:
                row = self.connection.execute(
                    f"SELECT Unit FROM Samples WHERE {where} AND Metric = ? ORDER BY SampleMs DESC LIMIT 1",
                    [*args, metric]).fetchone()
                item["unit"] = row[0] if row else None
                continue
            uwhere, uargs = self._where(node, [metric], since_ms, until_ms, True, unit)
            item["unit"] = unit
            item["min"], item["max"], item["mean"], used = self.connection.execute(
                f"SELECT MIN(Value), MAX(Value), AVG(Value), COUNT(*) FROM Samples WHERE {uwhere}", uargs).fetchone()
            item["excluded"] = item["usable"] - used
            item["latest"], item["latest_ms"] = self.connection.execute(
                f"SELECT Value, SampleMs FROM Samples WHERE {uwhere} ORDER BY SampleMs DESC LIMIT 1",
                uargs).fetchone()
            item["other_units"] = [row[0] for row in self.connection.execute(
                f"SELECT DISTINCT Unit FROM Samples WHERE {where} AND Metric = ? AND Value IS NOT NULL AND Unit != ? "
                f"ORDER BY Unit", [*args, metric, unit])]
        return out

    def series(self, node: str, metric: str, since_ms: int | None, until_ms: int | None,
               max_points: int = 500, unit: str | None = None) -> list[tuple[int, float]]:
        """(time, mean value) pairs in one unit (default: the latest), bucketed to at most max_points."""
        unit = unit or self.latest_unit(node, metric, since_ms, until_ms)
        if unit is None:
            return []
        where, args = self._where(node, [metric], since_ms, until_ms, True, unit)
        first, last = self.connection.execute(
            f"SELECT MIN(SampleMs), MAX(SampleMs) FROM Samples WHERE {where}", args).fetchone()
        if first is None:
            return []
        bucket = max(1, (last - first) // max(1, max_points - 1) + 1)
        return [(int(t), float(v)) for t, v in self.connection.execute(
            f"SELECT MIN(SampleMs), AVG(Value) FROM Samples WHERE {where} "
            f"GROUP BY (SampleMs - ?) / ? ORDER BY 1", [*args, first, bucket])]

    def rows(self, node: str, metrics: list[str] | None = None, since_ms: int | None = None,
             until_ms: int | None = None, healthy_only: bool = False, batch: int = 2000):
        """Streams export rows in time order, without loading everything into memory."""
        where, args = self._where(node, metrics, since_ms, until_ms, healthy_only)
        cursor = self.connection.execute(
            f"SELECT SampleMs, ReceivedMs, Node, Boot, Metric, Seq, Value, Unit, Health, Quality, CfgRev, "
            f"CalRev, Link FROM Samples WHERE {where} ORDER BY SampleMs, Metric", args)
        while chunk := cursor.fetchmany(batch):
            yield from chunk

    def boots(self, node: str, since_ms: int | None = None, until_ms: int | None = None) -> list[int]:
        where, args = self._where(node, None, since_ms, until_ms, False)
        return [row[0] for row in self.connection.execute(
            f"SELECT Boot, MIN(SampleMs) FROM Samples WHERE {where} GROUP BY Boot ORDER BY 2", args)]

    def config_revisions(self, node: str, since_ms: int | None = None, until_ms: int | None = None) -> list[int]:
        where, args = self._where(node, None, since_ms, until_ms, False)
        return [row[0] for row in self.connection.execute(
            f"SELECT DISTINCT CfgRev FROM Samples WHERE {where} ORDER BY CfgRev", args)]

    def recent_events(self, node: str, limit: int = 20) -> list[tuple]:
        return self.connection.execute(
            "SELECT EventMs, Rule, Level, Kind, Gap FROM Events WHERE Node = ? ORDER BY EventMs DESC LIMIT ?",
            (node, limit)).fetchall()

    def close(self) -> None:
        self.connection.close()

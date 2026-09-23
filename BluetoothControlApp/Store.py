"""Laptop-only telemetry history. WiFi credentials are never persisted here."""

import os
import sqlite3
from datetime import datetime, timezone
from pathlib import Path


class Store:
    def __init__(self, directory: Path | None = None):
        if directory is None:
            base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
            directory = base / "HydroponicsControlApp"
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        database = directory / "Readings.sqlite3"
        self.connection = sqlite3.connect(database)
        database.chmod(0o600)
        self.connection.execute("""CREATE TABLE IF NOT EXISTS Readings (
            Id INTEGER PRIMARY KEY, ReceivedUtc TEXT NOT NULL, Node TEXT NOT NULL,
            Device TEXT NOT NULL, Metric TEXT NOT NULL, Value REAL NOT NULL,
            Unit TEXT NOT NULL)""")
        self.connection.execute("CREATE INDEX IF NOT EXISTS ByDeviceTime ON Readings(Node, Device, ReceivedUtc)")
        self.connection.commit()

    def record(self, node: str, device: str, metric: str, value: float, unit: str) -> None:
        self.connection.execute(
            "INSERT INTO Readings(ReceivedUtc, Node, Device, Metric, Value, Unit) VALUES(?,?,?,?,?,?)",
            (datetime.now(timezone.utc).isoformat(), node, device, metric, float(value), unit),
        )
        self.connection.commit()

    def recent(self, limit: int = 10) -> list[tuple]:
        return self.connection.execute(
            "SELECT ReceivedUtc, Node, Device, Metric, Value, Unit FROM Readings ORDER BY Id DESC LIMIT ?",
            (limit,),
        ).fetchall()

    def count(self) -> int:
        return self.connection.execute("SELECT COUNT(*) FROM Readings").fetchone()[0]

    def close(self) -> None:
        self.connection.close()

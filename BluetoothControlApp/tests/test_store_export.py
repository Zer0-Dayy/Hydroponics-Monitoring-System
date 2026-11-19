import csv
import json

from Catalog import live_health, stale_after_s
from Export import export_meta, write_csv, write_json
from Store import Store


def sample(metric, seq, value, age_ms=0, health="OK", unit="ppm"):
    return {"metric": metric, "value": value, "unit": unit, "health": health, "age_ms": age_ms,
            "seq": seq, "quality": 0, "cal_rev": 1}


def test_dedupe_and_sample_time(tmp_path):
    store = Store(tmp_path)
    frame = [sample("tds", 1, 700.0, age_ms=2000), sample("tds", 2, 701.0, age_ms=1000), sample("tds", 3, 702.0)]
    assert store.record_samples("N", 7, 1, frame, "ble", received_ms=10_000) == 3
    # The next 3 s frame repeats seq 3; the same first frame also arrives over LAN.
    assert store.record_samples("N", 7, 1, [sample("tds", 3, 702.0, 3000), sample("tds", 4, 703.0)], "lan",
                                received_ms=13_000) == 1
    assert store.record_samples("N", 7, 1, frame, "lan", received_ms=10_050) == 0
    # A new boot restarts sequence numbers: not a duplicate.
    assert store.record_samples("N", 8, 1, [sample("tds", 1, 690.0)], "ble", received_ms=20_000) == 1
    rows = list(store.rows("N", ["tds"]))
    assert [r[0] for r in rows] == [8_000, 9_000, 10_000, 13_000, 20_000]  # SampleMs = received - age
    assert store.count("N") == 5
    store.close()


def test_never_measured_samples_are_not_stored(tmp_path):
    store = Store(tmp_path)
    assert store.record_samples("N", 1, 1, [{**sample("tvoc", 0, None), "age_ms": None}], "ble") == 0
    store.close()


def test_summary_series_and_filters(tmp_path):
    store = Store(tmp_path)
    for i in range(100):
        value = None if i % 10 == 0 else float(i)
        store.record_samples("N", 1, 2, [sample("tds", i, value, health="OK" if value else "STALE")], "ble",
                             received_ms=1_000 * i + 1)
    summary = store.summary("N", ["tds"])["tds"]
    assert summary["count"] == 100 and summary["usable"] == 90
    assert summary["min"] == 1.0 and summary["max"] == 99.0 and summary["latest"] == 99.0
    series = store.series("N", "tds", None, None, max_points=20)
    assert 1 < len(series) <= 20
    assert len(list(store.rows("N", ["tds"], since_ms=50_000, healthy_only=True))) == 45
    assert store.boots("N") == [1] and store.config_revisions("N") == [2]
    store.close()


def test_csv_and_json_export(tmp_path):
    store = Store(tmp_path)
    store.record_samples("N", 1, 1, [sample("tds", 1, 700.5), sample("ph", 1, None, health="FAULT")], "ble",
                         received_ms=1_700_000_000_000)
    csv_path, json_path = tmp_path / "out.csv", tmp_path / "out.json"
    assert write_csv(csv_path, store.rows("N")) == 2
    with open(csv_path, encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    assert rows[0]["sample_utc"] == "2023-11-14T22:13:20.000Z"
    assert {r["metric"]: r["value"] for r in rows} == {"tds": "700.5", "ph": ""}
    meta = export_meta("N", [], None, None, False, store.boots("N"), store.config_revisions("N"), "0.8.0")
    assert write_json(json_path, meta, store.rows("N")) == 2
    data = json.loads(json_path.read_text(encoding="utf-8"))
    assert data["meta"]["node"] == "N" and data["meta"]["boots"] == [1]
    assert {s["metric"]: s["value"] for s in data["samples"]} == {"tds": 700.5, "ph": None}
    store.close()


def test_settings_round_trip(tmp_path):
    store = Store(tmp_path)
    store.set_setting("N", "lan_token", "a" * 32)
    store.set_setting("N", "lan_token", "b" * 32)
    assert store.setting("N", "lan_token") == "b" * 32
    store.delete_settings("N", "lan_token")
    assert store.setting("N", "lan_token", "none") == "none"
    store.close()


def test_units_are_never_mixed_across_a_calibration(tmp_path):
    store = Store(tmp_path)
    store.record_samples("N", 1, 1, [sample("light_level", 1, 100.0, unit="mV", health="UNCALIBRATED")], "ble",
                         received_ms=1_000)
    store.record_samples("N", 1, 2, [sample("light_level", 2, 10_000.0, unit="lux")], "ble", received_ms=2_000)
    summary = store.summary("N", ["light_level"])["light_level"]
    assert summary["unit"] == "lux" and summary["mean"] == 10_000.0 and summary["min"] == 10_000.0
    assert summary["other_units"] == ["mV"] and summary["excluded"] == 1
    assert summary["count"] == 2 and summary["usable"] == 2
    assert [v for _t, v in store.series("N", "light_level", None, None)] == [10_000.0]
    assert [v for _t, v in store.series("N", "light_level", None, None, unit="mV")] == [100.0]
    store.close()


def test_fault_after_ok_is_a_new_row_when_seq_advances(tmp_path):
    # Firmware gives every observation its own seq (failures included), so the
    # FAULT that follows an OK sample is recorded, not dropped as a re-send.
    store = Store(tmp_path)
    assert store.record_samples("N", 1, 1, [sample("water_distance", 7, 120.0)], "ble", received_ms=1_000) == 1
    assert store.record_samples("N", 1, 1, [sample("water_distance", 7, 120.0)], "ble", received_ms=4_000) == 0
    assert store.record_samples("N", 1, 1, [sample("water_distance", 8, None, health="FAULT")], "ble",
                                received_ms=5_000) == 1
    summary = store.summary("N", ["water_distance"])["water_distance"]
    assert summary["count"] == 2 and summary["usable"] == 1
    store.close()


def test_old_values_age_into_stale_locally():
    assert live_health("air_temperature", "OK", 2.0) == "OK"
    assert live_health("air_temperature", "OK", 600.0) == "STALE"  # ten minutes old: not "Live"
    assert live_health("ph", "UNCALIBRATED", stale_after_s("ph") + 0.1) == "STALE"
    assert live_health("ph", "FAULT", 600.0) == "FAULT"
    assert stale_after_s("air_temperature") < stale_after_s("tds")

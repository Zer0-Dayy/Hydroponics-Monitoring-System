import json
import struct

import pytest

from Protocol import (POWER_MODES, Decoder, encode, parse_calibration, parse_diagnostics, parse_event, parse_state,
                      parse_telemetry, request)


def frame(obj) -> bytes:
    body = json.dumps(obj).encode()
    return struct.pack("<H", len(body)) + body


def test_decoder_reassembles_split_and_merged_frames():
    data = frame({"v": 2, "type": "state", "n": 1}) + frame({"v": 2, "type": "event", "n": 2})
    decoder = Decoder()
    out = []
    for i in range(0, len(data), 7):  # 7-byte packets split both frames
        out += decoder.feed(data[i:i + 7])
    assert [m["n"] for m in out] == [1, 2]


@pytest.mark.parametrize("packet", [struct.pack("<H", 0), struct.pack("<H", 4097)])
def test_decoder_rejects_bad_length(packet):
    with pytest.raises(ValueError):
        Decoder().feed(packet + b"x")


def test_decoder_rejects_v1_frames():
    with pytest.raises(ValueError):
        Decoder().feed(frame({"v": 1, "node": "old"}))


def test_request_shape_and_unknown_op():
    message = request("set_sensor_enabled", {"sensor": "tds", "enabled": False})
    assert message["v"] == 2 and message["op"] == "set_sensor_enabled" and len(message["id"]) == 36
    assert len(encode(message)) == struct.unpack_from("<H", encode(message))[0] + 2
    with pytest.raises(ValueError):
        request("add_device", {})


def test_parse_telemetry_validates_entries():
    samples = parse_telemetry({"v": 2, "type": "telemetry", "boot": 5, "cfg_rev": 1, "m": {
        "ph": {"value": 1532.0, "unit": "mV", "health": "UNCALIBRATED", "age_ms": 610, "seq": 7, "q": 0,
               "cal_rev": 0},
        "tds": {"value": None, "unit": "ppm", "health": "STALE", "age_ms": 9120, "seq": 3, "q": 0, "cal_rev": 1},
        "tvoc": {"value": "12", "unit": "ppb", "health": "OK", "age_ms": 10, "seq": 1},  # string value -> null
        "co2eq": {"value": 5, "health": "MADE_UP", "seq": 1},                             # unknown health
        "air_humidity": {"value": True, "health": "OK", "seq": -1},                       # bad seq
        "unknown_metric": {"value": 1, "health": "OK", "seq": 1},
    }})
    by_metric = {s["metric"]: s for s in samples}
    assert set(by_metric) == {"ph", "tds", "tvoc"}
    assert by_metric["ph"]["value"] == 1532.0 and by_metric["ph"]["unit"] == "mV"
    assert by_metric["tds"]["value"] is None
    assert by_metric["tvoc"]["value"] is None


def test_parse_state_defaults_and_rule_lists():
    state = parse_state({"v": 2, "type": "state", "node": "N", "boot": 9, "mode": "unknown",
                         "wifi": {"state": "connected", "ip": "10.0.0.2", "rssi": -60},
                         "safety": {"latched": [0, "x", 99], "relay": True}, "enabled": ["ph", 3]})
    assert state["wifi"]["ip"] == "10.0.0.2"
    assert state["safety"]["latched"] == [0] and state["safety"]["relay"] is True
    assert state["enabled"] == ["ph"]
    assert parse_state({"v": 2, "type": "state"})["wifi"]["state"] == "unconfigured"


def test_parse_event():
    event = parse_event({"v": 2, "type": "event", "n": 4, "rule": 0, "level": "CRITICAL", "kind": "trip",
                         "age_ms": 40, "gap": True})
    assert event["gap"] is True and event["n"] == 4
    with pytest.raises(ValueError):
        parse_event({"v": 2, "type": "event", "rule": 0})


def test_lan_ops_are_gone_and_calibration_ops_exist():
    for op in ("get_lan_token", "rotate_lan_token", "hello"):
        with pytest.raises(ValueError):
            request(op, {})
    for op in ("get_calibration", "cal_capture", "set_calibration", "reset_calibration", "get_diagnostics"):
        assert request(op, {})["op"] == op


def test_every_power_mode_is_kept_and_unknown_values_fall_back():
    for mode in POWER_MODES:
        assert parse_state({"v": 2, "type": "state", "mode": mode})["mode"] == mode
    assert parse_state({"v": 2, "type": "state", "mode": "sleeping"})["mode"] == "unknown"
    assert parse_state({"v": 2, "type": "state"})["mode"] == "unknown"


def test_parse_calibration_skips_malformed_entries():
    table = parse_calibration({"calibration": {
        "ph": {"overridden": True, "rev": 7, "valid": True, "slope": -0.0057, "offset": 22.7},
        "tds": {"overridden": False, "rev": 0, "valid": False, "slope": None, "offset": 0},
        "light": {"overridden": False, "rev": 0, "valid": False, "slope": 1, "offset": 0},
        "bme280": {"slope": 1, "offset": 0},
    }})
    assert set(table) == {"ph", "light"}
    assert table["ph"] == {"overridden": True, "rev": 7, "valid": True, "slope": -0.0057, "offset": 22.7}
    with pytest.raises(ValueError):
        parse_calibration({})


DIAGNOSTICS = {"uptime_ms": 734120, "reset_reason": "POWERON", "cpu_mhz": 240,
               "cpu": [{"load": 7, "peak": 19}, {"load": 12, "peak": 31}],
               "heap": {"total": 327680, "free": 182340, "min": 160112, "largest": 110592},
               "psram": {"total": 8388608, "free": 8350000},
               "flash": {"app_partition": 3145728, "nvs_used": 42, "nvs_total": 504},
               "stacks": [{"task": "Safety", "core": 1, "size": 3072, "free_min": 812}]}


def test_parse_diagnostics_keeps_valid_fields():
    d = parse_diagnostics(DIAGNOSTICS)
    assert d["uptime_ms"] == 734120 and d["reset_reason"] == "POWERON" and d["cpu_mhz"] == 240
    assert d["cpu"] == [{"load": 7, "peak": 19}, {"load": 12, "peak": 31}]
    assert d["heap"]["min"] == 160112 and d["psram"]["total"] == 8388608 and d["flash"]["nvs_used"] == 42
    assert d["stacks"] == [{"task": "Safety", "core": 1, "size": 3072, "free_min": 812}]


def test_parse_diagnostics_drops_malformed_sections():
    bad = {**DIAGNOSTICS,
           "cpu": [{"load": 140, "peak": 150}, {"load": 3, "peak": 4}],     # over 100 %
           "heap": {"total": 1000, "free": 2000, "min": 10, "largest": 10},  # more free than total
           "psram": {"total": -1, "free": 0},
           "flash": {"app_partition": "big"},
           "stacks": [{"task": "Safety", "core": 1, "size": 3072, "free_min": 9999},  # clamped to the size
                      {"task": "Ghost", "core": 7, "size": 1, "free_min": 0},        # no such core
                      {"core": 0, "size": 1024, "free_min": 10},                    # no name
                      "junk"],
           "reset_reason": None}
    d = parse_diagnostics(bad)
    assert d["cpu"] is None and d["heap"] is None and d["psram"] is None and d["flash"] is None
    assert d["stacks"] == [{"task": "Safety", "core": 1, "size": 3072, "free_min": 3072}]
    assert d["reset_reason"] == "UNKNOWN"


def test_parse_diagnostics_before_the_first_measurement():
    d = parse_diagnostics({"uptime_ms": 900, "reset_reason": "SW", "cpu_mhz": 240, "cpu": None, "heap": None,
                           "psram": {"total": 0, "free": 0},
                           "flash": {"app_partition": 0, "nvs_used": 0, "nvs_total": 0}, "stacks": []})
    assert d["cpu"] is None and d["heap"] is None and d["stacks"] == []
    with pytest.raises(ValueError):
        parse_diagnostics(["not", "a", "dict"])

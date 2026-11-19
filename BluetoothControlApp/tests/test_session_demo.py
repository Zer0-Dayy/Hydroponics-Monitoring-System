"""Session behaviour against the demo controller (no hardware needed)."""

import asyncio

import pytest

import Session as session_module
from DemoController import demo_ble_targets
from Session import Session
from Store import Store


@pytest.fixture(autouse=True)
def fast_retries(monkeypatch):
    monkeypatch.setattr(session_module, "BLE_RETRY_DELAYS", (0.2, 0.2, 0.2))


async def wait_for(predicate, timeout=10.0):
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while not predicate():
        if loop.time() > deadline:
            raise AssertionError("condition not reached")
        await asyncio.sleep(0.05)


async def connected_session(tmp_path) -> Session:
    s = Session(Store(tmp_path), demo=True)
    await s.connect_ble(demo_ble_targets(s.demo_controller)[0])
    return s


def test_session_has_no_wifi_link():
    assert not hasattr(Session, "connect_lan")
    assert not hasattr(Session, "set_preference")


def test_ble_connect_records_and_commands(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        assert s.active_link == "ble" and s.can_command
        await wait_for(lambda: s.recorded > 0)
        assert set(s.calibration) == {"ph", "tds", "light"}  # fetched on connect
        await s.set_sensor_enabled("tds", False)
        await wait_for(lambda: "tds" not in s.enabled)
        await s.close()
    asyncio.run(scenario())


def test_wifi_setting_does_not_open_a_link(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        await s.set_wifi("Greenhouse", "longpassword")
        await wait_for(lambda: s.state["wifi"]["state"] == "connected")
        assert s.active_link == "ble"
        await s.close()
    asyncio.run(scenario())


def test_ble_lost_then_reconnects(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        s.demo_controller.set_ble_in_range(False)
        await wait_for(lambda: not s.ble.connected)
        assert not s.can_command
        s.demo_controller.set_ble_in_range(True)
        await wait_for(lambda: s.ble.connected)
        await s.close()
    asyncio.run(scenario())


def test_link_lost_during_reconnect_handshake_is_retried(tmp_path, monkeypatch):
    """A loss inside the reconnect's first get_sensors must not end the retries
    (the old code announced "Bluetooth reconnected." and stayed offline)."""
    from DemoController import DemoLink

    async def scenario():
        s = await connected_session(tmp_path)
        original = DemoLink.send
        armed = {"once": True}

        async def send(self, operation, data):
            if armed["once"] and operation == "get_sensors":
                armed["once"] = False
                self.drop("Bluetooth connection lost")
                raise ConnectionError("Bluetooth connection lost")
            return await original(self, operation, data)

        monkeypatch.setattr(DemoLink, "send", send)
        notices = []
        s.notify = notices.append
        s.ble.drop("Bluetooth connection lost")
        await wait_for(lambda: not armed["once"])  # the handshake loss happened
        await wait_for(lambda: s.ble.connected and not s.ble_reconnecting)
        assert notices.count("Bluetooth reconnected.") == 1
        await s.close()
    asyncio.run(scenario())


def test_disable_refused_while_alarm_latched(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        s.demo_controller.set_heat(True)
        await wait_for(lambda: s.state and s.state["safety"]["latched"] == [0])
        with pytest.raises(RuntimeError, match="rule_latched"):
            await s.set_sensor_enabled("bme280", False)
        with pytest.raises(RuntimeError, match="rule_active"):
            await s.reset_latch(0)
        s.demo_controller.set_heat(False)
        await wait_for(lambda: s.state["safety"]["active"] == [])
        await s.reset_latch(0)
        await wait_for(lambda: s.state["safety"]["latched"] == [])
        await s.close()
    asyncio.run(scenario())


def test_ph_two_point_calibration_then_reset(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        await wait_for(lambda: s.readings.get("ph", {}).get("health") == "UNCALIBRATED")
        mv = await s.cal_capture("ph")
        assert 2500 < mv < 2800
        rev = await s.set_calibration("ph", [(2500.0, 7.0), (3030.0, 4.0)])
        assert rev > 1
        assert s.calibration["ph"]["overridden"] and s.calibration["ph"]["rev"] == rev
        await wait_for(lambda: s.readings["ph"]["health"] == "OK" and s.readings["ph"]["cal_rev"] == rev)
        await s.reset_calibration("ph")
        assert not s.calibration["ph"]["overridden"]
        await s.close()
    asyncio.run(scenario())


def test_calibration_refuses_implausible_fit(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        with pytest.raises(RuntimeError, match="bad_fit"):
            await s.set_calibration("ph", [(2500.0, 7.0), (2510.0, 4.0)])  # far too steep
        with pytest.raises(RuntimeError, match="bad_fit"):
            await s.set_calibration("tds", [(100.0, 707.0)])  # K = 7
        await s.close()
    asyncio.run(scenario())


def test_light_lux_calibration_changes_unit(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        mv = await s.cal_capture("light")
        await s.set_calibration("light", [(mv, 3000.0)])
        await wait_for(lambda: s.readings.get("light_level", {}).get("unit") == "lux")
        assert s.readings["light_level"]["health"] == "OK"
        await s.close()
    asyncio.run(scenario())


def test_low_power_mode_round_trip(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        s.demo_controller.set_external_power(False)
        await wait_for(lambda: s.power_mode == "low_power")
        await wait_for(lambda: s.readings.get("ph", {}).get("health") == "UNPOWERED")
        with pytest.raises(RuntimeError, match="stale"):
            await s.cal_capture("ph")  # its 5 V rail is off
        assert s.can_command  # Bluetooth stays up in low power
        s.demo_controller.set_external_power(True)
        await wait_for(lambda: s.power_mode == "normal")
        await s.close()
    asyncio.run(scenario())


def test_diagnostics_from_the_demo_and_reset_on_disconnect(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        assert s.diagnostics_due and s.diagnostics is None
        await s.refresh_diagnostics()
        d = s.diagnostics
        assert s.diagnostics_fresh and not s.diagnostics_due
        assert len(d["cpu"]) == 2 and d["heap"]["free"] <= d["heap"]["total"]
        assert {item["task"] for item in d["stacks"]} >= {"Safety", "AcqNormal", "BleTransport"}
        await s.disconnect()
        assert s.diagnostics is None and not s.diagnostics_due
        await s.close()
    asyncio.run(scenario())


def test_old_firmware_without_diagnostics(tmp_path):
    async def scenario():
        s = await connected_session(tmp_path)
        original = s.demo_controller.command

        async def older(operation, data):
            if operation == "get_diagnostics":
                raise RuntimeError("unknown_op")
            return await original(operation, data)
        s.demo_controller.command = older
        await s.refresh_diagnostics()
        assert not s.diagnostics_supported and s.diagnostics is None and not s.diagnostics_due
        await s.close()
    asyncio.run(scenario())

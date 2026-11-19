"""BleLink after a link loss (hardware finding with env:ble_tear, 28 Oct 2026):
a lost client is closed (on Windows its GATT session otherwise keeps the bonded
link up), the decoders forget a cut frame, and an old session's notifications
are never mixed into the current stream."""
import asyncio

from BleLink import BleLink
from Protocol import STATE_UUID, TELEMETRY_UUID, encode


class FakeClient:
    def __init__(self):
        self.closed = 0

    async def disconnect(self):
        self.closed += 1


def frame(message: dict) -> bytes:
    return encode({"v": 2, **message})


def test_lost_link_closes_client_and_clears_cut_frame():
    async def run():
        messages, reasons = [], []
        link = BleLink(lambda _l, m: messages.append(m), lambda _l, r: reasons.append(r))
        client = FakeClient()
        link.client = link.listening = client
        link.connected = True
        whole = frame({"type": "telemetry", "m": {}})
        link._notification(client, TELEMETRY_UUID, whole[:5])  # first chunk only, then the link drops
        link._disconnected(client)
        await asyncio.sleep(0)
        assert client.closed == 1 and reasons == ["Bluetooth connection lost"]
        assert not link.connected and link.client is None
        assert all(len(d.buffer) == 0 for d in link.decoders.values())

        fresh = FakeClient()
        link.client = link.listening = fresh
        link.connected = True
        link._notification(fresh, TELEMETRY_UUID, whole)  # the next frame decodes on its own
        assert messages == [{"v": 2, "type": "telemetry", "m": {}}]

    asyncio.run(run())


def test_old_session_notifications_and_callbacks_are_ignored():
    async def run():
        messages, reasons = [], []
        link = BleLink(lambda _l, m: messages.append(m), lambda _l, r: reasons.append(r))
        old, current = FakeClient(), FakeClient()
        link.client = link.listening = current
        link.connected = True
        link._notification(old, STATE_UUID, frame({"type": "state"})[:4])  # stale half frame
        link._disconnected(old)  # late callback from the old client
        assert link.connected and reasons == [] and messages == []
        link._notification(current, STATE_UUID, frame({"type": "state"}))
        assert messages == [{"v": 2, "type": "state"}]

    asyncio.run(run())


def test_disconnect_closes_even_a_client_that_is_no_longer_connected():
    async def run():
        link = BleLink(lambda *_: None, lambda *_: None)
        client = FakeClient()
        link.client = client
        await link.disconnect()
        assert client.closed == 1

    asyncio.run(run())


class FakeServices:
    def get_characteristic(self, _uuid):
        return object()


class ConnectingClient(FakeClient):
    """Stands in for BleakClient: records how it was built and what was asked."""
    made = []

    def __init__(self, device, **kwargs):
        super().__init__()
        self.device, self.kwargs = device, kwargs
        self.services = FakeServices()
        self.reads, self.notifies = [], []
        ConnectingClient.made.append(self)

    async def connect(self):
        pass

    async def start_notify(self, uuid, _callback):
        self.notifies.append(uuid)

    async def read_gatt_char(self, uuid):
        self.reads.append(uuid)
        return frame({"type": "state"})


def test_connect_needs_no_passkey_and_clears_a_stale_os_pairing(monkeypatch):
    import BleLink as module
    from Links import Target
    from Protocol import RESULT_UUID

    calls = []

    async def remove_stale_bond(device):
        calls.append(device)
        return False

    monkeypatch.setattr(module, "BleakClient", ConnectingClient)
    monkeypatch.setattr(module.OsBond, "remove_stale_bond", remove_stale_bond)
    ConnectingClient.made.clear()

    async def run():
        messages = []
        link = BleLink(lambda _l, m: messages.append(m), lambda *_: None)
        await link.connect(Target("ble", "Hydro-ESP1", "AA:BB:CC:DD:EE:FF"))
        client = ConnectingClient.made[0]
        assert calls == ["AA:BB:CC:DD:EE:FF"]  # the OS pairing check runs before every connect
        assert client.kwargs["pair"] is False
        assert client.kwargs["winrt"] == {"use_cached_services": False}
        assert client.notifies == [STATE_UUID, TELEMETRY_UUID, RESULT_UUID]
        assert client.reads == [STATE_UUID]  # no authentication read any more
        assert link.connected and messages == [{"v": 2, "type": "state"}]

    asyncio.run(run())


def test_os_bond_check_never_raises(monkeypatch):
    import OsBond

    async def broken(_device):
        raise OSError("no Bluetooth adapter")

    monkeypatch.setattr(OsBond, "_windows_unpair", broken)
    monkeypatch.setattr(OsBond, "_bluez_remove", broken)
    assert asyncio.run(OsBond.remove_stale_bond("AA:BB:CC:DD:EE:FF")) is False

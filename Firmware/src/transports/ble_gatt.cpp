// NimBLE-Arduino implementation of ble_gatt.h. The only C++ file in the
// project: NimBLE-Arduino is a C++ library, and ESP-Demo already proved this
// version on espressif32@6.12.0 / ESP32-S3. Everything it exports is C.
//
// Compiled only with -D FW_ENABLE_BLE (the production envs), so the bring-up
// images do not link NimBLE.
#ifdef FW_ENABLE_BLE
#include "transports/ble_gatt.h"

#include <NimBLEDevice.h>

#include <atomic>
#include <string.h>

namespace {

constexpr const char* kServiceUuid = "98a90f00-346e-4e65-9cbd-7687685bdc01";
constexpr const char* kStateUuid = "98a90f01-346e-4e65-9cbd-7687685bdc01";
constexpr const char* kTelemetryUuid = "98a90f02-346e-4e65-9cbd-7687685bdc01";
constexpr const char* kCommandUuid = "98a90f03-346e-4e65-9cbd-7687685bdc01";
constexpr const char* kResultUuid = "98a90f04-346e-4e65-9cbd-7687685bdc01";
constexpr uint16_t kPreferredMtu = 247;
constexpr size_t kMinChunk = 20;
constexpr uint8_t kAllChannels = (1u << BLE_CHANNEL_COUNT) - 1u;
// Connection parameters asked for after connect (the central may refuse):
// 15-30 ms interval, no latency, 4 s supervision timeout, so a client that
// vanishes (laptop closed, out of range) frees the controller within ~4 s.
constexpr uint16_t kConnIntervalMin = 12;  // x 1.25 ms
constexpr uint16_t kConnIntervalMax = 24;
constexpr uint16_t kSupervisionTimeout = 400;  // x 10 ms

NimBLEServer* s_server = nullptr;
NimBLECharacteristic* s_channel[BLE_CHANNEL_COUNT] = {};
NimBLECharacteristic* s_command = nullptr;
BleRxFn s_rx = nullptr;
void* s_rxCtx = nullptr;

std::atomic<uint32_t> s_generation{0};
std::atomic<uint16_t> s_conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint16_t> s_mtu{23};
std::atomic<uint8_t> s_subscribed{0};  // bit per BleChannel, current client only

portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
BleGattStats s_stats = {};

template <typename F>
void stats(F update) {
  portENTER_CRITICAL(&s_statsMux);
  update(s_stats);
  portEXIT_CRITICAL(&s_statsMux);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    if (s_conn.load() != BLE_HS_CONN_HANDLE_NONE || server->getConnectedCount() > 1) {  // one client at a time
      stats([](BleGattStats& s) { ++s.refused; });
      server->disconnect(info.getConnHandle());
      return;
    }
    s_subscribed.store(0);
    s_mtu.store(info.getMTU() > 23 ? info.getMTU() : 23);
    s_conn.store(info.getConnHandle());
    s_generation.fetch_add(1);
    stats([](BleGattStats& s) { ++s.connects; });
    server->updateConnParams(info.getConnHandle(), kConnIntervalMin, kConnIntervalMax, 0, kSupervisionTimeout);
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (info.getConnHandle() == s_conn.load()) {
      s_subscribed.store(0);
      s_conn.store(BLE_HS_CONN_HANDLE_NONE);
      s_generation.fetch_add(1);
    }
    stats([reason](BleGattStats& s) {
      ++s.disconnects;
      s.lastDisconnectReason = reason;
    });
    // Advertise again once the slot is free (not after refusing a second client).
    if (s_conn.load() == BLE_HS_CONN_HANDLE_NONE) NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    if (info.getConnHandle() == s_conn.load()) s_mtu.store(mtu);
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (info.getConnHandle() != s_conn.load()) return;  // not the current client
    const NimBLEAttValue value = characteristic->getValue();
    if (s_rx != nullptr && value.length() > 0) s_rx(value.data(), value.length(), s_generation.load(), s_rxCtx);
  }
};

// One instance per notify channel: records whether the current client listens.
class ChannelCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ChannelCallbacks(BleChannel channel) : m_bit(static_cast<uint8_t>(1u << channel)) {}

  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
    if (info.getConnHandle() != s_conn.load()) return;
    if (subValue != 0) {
      s_subscribed.fetch_or(m_bit);
    } else {
      s_subscribed.fetch_and(static_cast<uint8_t>(~m_bit));
    }
  }

 private:
  uint8_t m_bit;
};

ServerCallbacks s_serverCallbacks;
CommandCallbacks s_commandCallbacks;
ChannelCallbacks s_channelCallbacks[BLE_CHANNEL_COUNT] = {
    ChannelCallbacks(BLE_CHANNEL_STATE), ChannelCallbacks(BLE_CHANNEL_TELEMETRY), ChannelCallbacks(BLE_CHANNEL_RESULT)};

void dropCurrent() {
  if (s_server == nullptr) return;
  const uint16_t conn = s_conn.load();
  if (conn != BLE_HS_CONN_HANDLE_NONE) s_server->disconnect(conn);
}

}  // namespace

extern "C" bool bleGattStart(const char* deviceName, BleRxFn rx, void* rxCtx) {
  if (s_server != nullptr) return true;
  s_rx = rx;
  s_rxCtx = rxCtx;
  if (!NimBLEDevice::init(deviceName)) return false;
  // Open link: no pairing is asked for and nothing is bonded. If a computer
  // pairs on its own anyway, it gets an unbonded "Just Works" link.
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // Bonds stored by firmware <= 0.10 would otherwise linger in NVS.
  if (NimBLEDevice::getNumBonds() > 0) NimBLEDevice::deleteAllBonds();
  NimBLEDevice::setMTU(kPreferredMtu);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(&s_serverCallbacks);
  NimBLEService* service = s_server->createService(kServiceUuid);
  s_channel[BLE_CHANNEL_STATE] = service->createCharacteristic(
      kStateUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, BLE_GATT_STATE_MAX);
  s_channel[BLE_CHANNEL_TELEMETRY] =
      service->createCharacteristic(kTelemetryUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 20);
  s_command = service->createCharacteristic(kCommandUuid, NIMBLE_PROPERTY::WRITE, 512);
  s_command->setCallbacks(&s_commandCallbacks);
  s_channel[BLE_CHANNEL_RESULT] =
      service->createCharacteristic(kResultUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 20);
  for (int c = 0; c < BLE_CHANNEL_COUNT; ++c) s_channel[c]->setCallbacks(&s_channelCallbacks[c]);
  if (!s_server->start()) return false;

  // 31-byte advertising packet: flags + 128-bit service UUID (21 B) leave no room
  // for the name, so the name goes in the scan response. Order matters:
  // setName() only targets the scan response once it is enabled.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->setName(deviceName);
  advertising->addServiceUUID(kServiceUuid);
  return advertising->start();
}

extern "C" bool bleGattConnected(void) { return s_conn.load() != BLE_HS_CONN_HANDLE_NONE; }

extern "C" bool bleGattReady(void) { return bleGattConnected() && s_subscribed.load() == kAllChannels; }

extern "C" uint32_t bleGattGeneration(void) { return s_generation.load(); }

extern "C" size_t bleGattChunkSize(void) {
  const size_t mtu = s_mtu.load();
  return mtu > kMinChunk + 3 ? mtu - 3 : kMinChunk;
}

extern "C" bool bleGattNotify(BleChannel channel, const uint8_t* data, size_t len) {
  const uint16_t conn = s_conn.load();
  if (channel >= BLE_CHANNEL_COUNT || conn == BLE_HS_CONN_HANDLE_NONE || (s_subscribed.load() & (1u << channel)) == 0 ||
      len == 0 || len > bleGattChunkSize()) {
    return false;
  }
  if (!s_channel[channel]->notify(data, len, conn)) {
    stats([](BleGattStats& s) { ++s.notifyFailures; });
    return false;
  }
  return true;
}

extern "C" bool bleGattSetStateValue(const uint8_t* frame, size_t len) {
  if (s_channel[BLE_CHANNEL_STATE] == nullptr || len > BLE_GATT_STATE_MAX) return false;
  s_channel[BLE_CHANNEL_STATE]->setValue(frame, len);
  return true;
}

extern "C" void bleGattDisconnect(void) { dropCurrent(); }

extern "C" void bleGattDropIdle(void) {
  if (!bleGattConnected()) return;
  stats([](BleGattStats& s) { ++s.idleDropped; });
  dropCurrent();
}

extern "C" void bleGattGetStats(BleGattStats* out) {
  portENTER_CRITICAL(&s_statsMux);
  *out = s_stats;
  portEXIT_CRITICAL(&s_statsMux);
}

#endif  // FW_ENABLE_BLE

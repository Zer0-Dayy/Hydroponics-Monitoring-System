#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <new>
#include <vector>

namespace {
constexpr char ServiceUuid[] = "98a90f00-346e-4e65-9cbd-7687685bdc01";
constexpr char StateUuid[] = "98a90f01-346e-4e65-9cbd-7687685bdc01";
constexpr char TelemetryUuid[] = "98a90f02-346e-4e65-9cbd-7687685bdc01";
constexpr char CommandUuid[] = "98a90f03-346e-4e65-9cbd-7687685bdc01";
constexpr char ResultUuid[] = "98a90f04-346e-4e65-9cbd-7687685bdc01";
constexpr char AuthUuid[] = "98a90f05-346e-4e65-9cbd-7687685bdc01";
constexpr char NodeName[] = "ESP1 Demo";
constexpr size_t MaxMessage = 4096;
constexpr uint8_t BootPin = 0;
constexpr unsigned long StateIntervalMs = 2000;
constexpr unsigned long TelemetryIntervalMs = 3000;
constexpr unsigned long RxTimeoutMs = 5000;

struct Device {
    String id;
    String type;
    String label;
};

struct DeviceType {
    const char* type;
    const char* label;
    const char* metric;
    const char* unit;
    float baseline;
    float jitter;
    bool essential;
    bool actuator;
};

constexpr DeviceType Catalog[] = {
    {"level", "Reservoir level", "Reservoir level", "cm", 24.0f, 0.4f, true, false},
    {"ph", "Solution pH", "Solution pH", "pH", 6.1f, 0.08f, true, false},
    {"tds", "Nutrient strength", "Nutrient strength", "ppm", 740.0f, 8.0f, true, false},
    {"climate", "Air temperature & humidity", "Air temperature & humidity", "C", 25.0f, 0.3f, false, false},
    {"light", "Ambient light", "Ambient light", "raw", 780.0f, 20.0f, false, false},
    {"gas", "CO sensor", "CO sensor", "raw", 0.0f, 0.0f, false, false},
    {"relay1", "Relay 1", "Relay 1", "on/off", 0.0f, 0.0f, false, true},
    {"relay2", "Relay 2", "Relay 2", "on/off", 0.0f, 0.0f, false, true},
    {"relay3", "Relay 3", "Relay 3", "on/off", 0.0f, 0.0f, false, true},
    {"relay4", "Relay 4", "Relay 4", "on/off", 0.0f, 0.0f, false, true},
};

struct CommandMessage {
    char json[MaxMessage + 1];
};

Preferences preferences;
std::vector<Device> devices;
String wifiSsid;
String wifiPassword;
String ownerAddress;
std::atomic<bool> lowPower{false};
std::atomic<bool> authenticated{false};
std::atomic<bool> telemetrySubscribed{false};
NimBLEServer* server = nullptr;
NimBLECharacteristic* stateCharacteristic = nullptr;
NimBLECharacteristic* telemetryCharacteristic = nullptr;
NimBLECharacteristic* commandCharacteristic = nullptr;
NimBLECharacteristic* resultCharacteristic = nullptr;
QueueHandle_t commandQueue = nullptr;
uint32_t pairingPasskey = 0;
unsigned long lastStateMs = 0;
unsigned long lastTelemetryMs = 0;

const DeviceType* findType(const String& type) {
    for (const auto& item : Catalog) {
        if (type == item.type) return &item;
    }
    return nullptr;
}

bool isActive(const DeviceType& item) {
    return !lowPower.load() || item.essential;
}

String framedJson(const String& json) {
    if (json.length() == 0 || json.length() > MaxMessage) return "";
    String frame;
    frame.reserve(json.length() + 2);
    const uint16_t size = static_cast<uint16_t>(json.length());
    frame += static_cast<char>(size & 0xff);
    frame += static_cast<char>((size >> 8) & 0xff);
    frame += json;
    return frame;
}

void notifyFrame(NimBLECharacteristic* characteristic, const String& json) {
    if (!characteristic || !authenticated.load() || server->getConnectedCount() == 0) return;
    const String frame = framedJson(json);
    if (frame.isEmpty()) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame.c_str());
    for (size_t offset = 0; offset < frame.length(); offset += 20) {
        const size_t count = std::min(static_cast<size_t>(20), frame.length() - offset);
        if (!characteristic->notify(bytes + offset, count)) break;
        delay(8);
    }
}

String currentWifiStatus() {
    if (lowPower.load()) return "disabled";
    if (wifiSsid.isEmpty()) return "unconfigured";
    return WiFi.status() == WL_CONNECTED ? "connected" : "connecting";
}

String stateJson() {
    DynamicJsonDocument document(512);
    document["v"] = 1;
    document["node"] = NodeName;
    document["mode"] = lowPower.load() ? "low_power" : "normal";
    document["wifi"] = currentWifiStatus();
    document["battery_percent"] = 74;  // Simulated, not a battery measurement.
    JsonArray active = document.createNestedArray("active_types");
    for (const auto& item : Catalog) {
        if (isActive(item)) active.add(item.type);
    }
    String json;
    serializeJson(document, json);
    return json;
}

void publishState() {
    const String frame = framedJson(stateJson());
    if (stateCharacteristic && !frame.isEmpty()) {
        stateCharacteristic->setValue(
            reinterpret_cast<const uint8_t*>(frame.c_str()), frame.length());
        notifyFrame(stateCharacteristic, stateJson());
    }
}

bool persistDevices() {
    DynamicJsonDocument document(2048);
    JsonArray list = document.to<JsonArray>();
    for (const auto& item : devices) {
        JsonObject entry = list.createNestedObject();
        entry["id"] = item.id;
        entry["type"] = item.type;
        entry["label"] = item.label;
    }
    String json;
    serializeJson(document, json);
    return preferences.putString("devices", json) == json.length();
}

void loadDevices() {
    const String saved = preferences.getString("devices", "");
    if (!saved.isEmpty()) {
        DynamicJsonDocument document(2048);
        if (!deserializeJson(document, saved) && document.is<JsonArray>()) {
            for (JsonObject entry : document.as<JsonArray>()) {
                String type = entry["type"].as<String>();
                const DeviceType* kind = findType(type);
                if (kind && devices.size() < 10) {
                    devices.push_back({entry["id"].as<String>(), type, kind->label});
                }
            }
            return;
        }
    }
    for (const char* type : {"level", "ph", "tds", "climate"}) {
        const DeviceType* kind = findType(type);
        devices.push_back({String("esp1-") + type, type, kind->label});
    }
    persistDevices();
}

void beginWifi() {
    if (lowPower.load() || wifiSsid.isEmpty()) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
}

void setLowPower(bool enabled) {
    if (lowPower.load() == enabled) return;
    lowPower.store(enabled);
    if (enabled) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    } else {
        beginWifi();
    }
    Serial.printf("Mode: %s\n", enabled ? "LOW POWER" : "NORMAL");
    publishState();
}

void sendResult(const String& id, const String& status, const String& error, JsonVariantConst data = JsonVariantConst()) {
    DynamicJsonDocument document(4096);
    document["v"] = 1;
    document["id"] = id;
    document["status"] = status;
    if (status == "ok") {
        if (!data.isNull()) document["data"] = data;
        else document.createNestedObject("data");
    } else {
        document["error"] = error;
    }
    String json;
    serializeJson(document, json);
    notifyFrame(resultCharacteristic, json);
}

void processCommand(const char* payload) {
    DynamicJsonDocument request(4096);
    const DeserializationError parseError = deserializeJson(request, payload);
    if (parseError || request["v"] != 1 || !request["id"].is<const char*>() ||
        !request["op"].is<const char*>()) {
        return;  // Malformed frames have no trustworthy correlation ID.
    }
    const String id = request["id"].as<String>();
    if (id.length() != 36) return;
    const String operation = request["op"].as<String>();
    JsonVariantConst data = request["data"];
    if (operation == "get_devices") {
        DynamicJsonDocument response(3072);
        JsonArray list = response.createNestedArray("devices");
        for (const auto& item : devices) {
            JsonObject entry = list.createNestedObject();
            entry["id"] = item.id;
            entry["type"] = item.type;
            entry["label"] = item.label;
            entry["online"] = true;  // Simulated presence.
        }
        sendResult(id, "ok", "", response.as<JsonVariantConst>());
        return;
    }
    if (operation == "set_wifi") {
        if (lowPower.load()) {
            sendResult(id, "error", "WiFi is disabled in low-power mode");
            return;
        }
        const String ssid = data["ssid"].as<String>();
        const String password = data["password"].as<String>();
        if (ssid.isEmpty() || ssid.length() > 32 || password.length() < 8 ||
            password.length() > 63) {
            sendResult(id, "error", "Invalid WiFi settings");
            return;
        }
        const String oldSsid = wifiSsid;
        const String oldPassword = wifiPassword;
        if (preferences.putString("ssid", ssid) != ssid.length() ||
            preferences.putString("password", password) != password.length()) {
            preferences.putString("ssid", oldSsid);
            preferences.putString("password", oldPassword);
            sendResult(id, "error", "Could not save WiFi settings");
            return;
        }
        wifiSsid = ssid;
        wifiPassword = password;
        DynamicJsonDocument response(64);
        response["accepted"] = true;
        sendResult(id, "ok", "", response.as<JsonVariantConst>());
        beginWifi();
        publishState();
        return;
    }
    if (operation == "add_device") {
        const String type = data["type"].as<String>();
        const DeviceType* kind = findType(type);
        if (!kind || devices.size() >= 10 || !isActive(*kind)) {
            sendResult(id, "error", "Unsupported or unavailable device");
            return;
        }
        for (const auto& item : devices) {
            if (item.type == type) {
                sendResult(id, "error", "Device is already configured");
                return;
            }
        }
        devices.push_back({String("esp1-") + type, type, kind->label});
        if (!persistDevices()) {
            devices.pop_back();
            sendResult(id, "error", "Could not save device");
            return;
        }
        DynamicJsonDocument response(256);
        JsonObject entry = response.createNestedObject("device");
        entry["id"] = devices.back().id;
        entry["type"] = type;
        entry["label"] = kind->label;
        entry["online"] = true;
        sendResult(id, "ok", "", response.as<JsonVariantConst>());
        return;
    }
    if (operation == "delete_device") {
        const String deviceId = data["id"].as<String>();
        for (size_t index = 0; index < devices.size(); ++index) {
            if (devices[index].id != deviceId) continue;
            const DeviceType* kind = findType(devices[index].type);
            if (!kind || !isActive(*kind)) {
                sendResult(id, "error", "Device is unavailable in low-power mode");
                return;
            }
            const Device removed = devices[index];
            devices.erase(devices.begin() + index);
            if (!persistDevices()) {
                devices.insert(devices.begin() + index, removed);
                sendResult(id, "error", "Could not save device removal");
                return;
            }
            DynamicJsonDocument response(64);
            response["deleted"] = true;
            sendResult(id, "ok", "", response.as<JsonVariantConst>());
            return;
        }
        sendResult(id, "error", "Device was not found");
        return;
    }
    sendResult(id, "error", "Unsupported operation");
}

void publishTelemetry() {
    if (!telemetrySubscribed.load() || !authenticated.load()) return;
    for (const auto& device : devices) {
        if (!telemetrySubscribed.load()) break;
        const DeviceType* kind = findType(device.type);
        if (!kind || kind->actuator || !isActive(*kind)) continue;
        const float fraction = static_cast<float>(esp_random() % 2001) / 1000.0f - 1.0f;
        const float value = kind->baseline + fraction * kind->jitter;
        DynamicJsonDocument document(256);
        document["v"] = 1;
        document["node"] = NodeName;
        document["device"] = device.id;
        document["metric"] = kind->metric;
        document["value"] = roundf(value * 100.0f) / 100.0f;
        document["unit"] = kind->unit;
        String json;
        serializeJson(document, json);
        notifyFrame(telemetryCharacteristic, json);
    }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* connectedServer, NimBLEConnInfo& info) override {
        Serial.println("BLE client connected; awaiting authenticated pairing");
        if (connectedServer->getConnectedCount() > 1) {
            connectedServer->disconnect(info.getConnHandle());
        }
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("BLE client disconnected (reason %d)\n", reason);
        authenticated.store(false);
        telemetrySubscribed.store(false);
        NimBLEDevice::startAdvertising();
    }

    uint32_t onPassKeyDisplay() override {
        Serial.printf("BLE pairing passkey: %06u\n", pairingPasskey);
        return pairingPasskey;
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        bool secure = info.isEncrypted() && info.isAuthenticated() &&
                      info.isBonded() && info.getSecKeySize() >= 16;
        const String identity = info.getIdAddress().toString().c_str();
        Serial.printf("BLE authentication: encrypted=%d authenticated=%d bonded=%d key_bytes=%d\n",
                      info.isEncrypted(), info.isAuthenticated(), info.isBonded(), info.getSecKeySize());
        if (secure && !ownerAddress.isEmpty() && identity != ownerAddress) {
            Serial.println("BLE owner mismatch; rejecting new laptop");
            NimBLEDevice::deleteBond(info.getIdAddress());
            secure = false;
        }
        if (secure && ownerAddress.isEmpty()) {
            secure = preferences.putString("owner", identity) == identity.length();
            if (secure) ownerAddress = identity;
        }
        authenticated.store(secure);
        Serial.printf("BLE owner authentication %s\n", secure ? "accepted" : "rejected");
        if (!secure) server->disconnect(info.getConnHandle());
    }
};

class TelemetryCallbacks final : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t value) override {
        telemetrySubscribed.store(value != 0 && authenticated.load() &&
                                  info.isEncrypted() && info.isAuthenticated() && info.isBonded());
    }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
    std::vector<uint8_t> received;
    unsigned long lastChunk = 0;

    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        if (!authenticated.load() || !info.isEncrypted() ||
            !info.isAuthenticated() || !info.isBonded()) {
            server->disconnect(info.getConnHandle());
            return;
        }
        const unsigned long now = millis();
        if (now - lastChunk > RxTimeoutMs) received.clear();
        lastChunk = now;
        const std::string chunk = characteristic->getValue();
        if (received.size() + chunk.size() > MaxMessage + 2) {
            received.clear();
            server->disconnect(info.getConnHandle());
            return;
        }
        received.insert(received.end(), chunk.begin(), chunk.end());
        while (received.size() >= 2) {
            const size_t length = received[0] | (static_cast<size_t>(received[1]) << 8);
            if (length == 0 || length > MaxMessage) {
                received.clear();
                server->disconnect(info.getConnHandle());
                return;
            }
            if (received.size() < length + 2) break;
            auto* message = new (std::nothrow) CommandMessage;
            if (message) {
                memcpy(message->json, received.data() + 2, length);
                message->json[length] = 0;
                if (xQueueSend(commandQueue, &message, 0) != pdTRUE) delete message;
            }
            received.erase(received.begin(), received.begin() + length + 2);
        }
    }
};

ServerCallbacks serverCallbacks;
TelemetryCallbacks telemetryCallbacks;
CommandCallbacks commandCallbacks;

void setupBle() {
    pairingPasskey = 100000 + esp_random() % 900000;
    Serial.printf("BLE pairing passkey: %06u\n", pairingPasskey);
    NimBLEDevice::init("Hydroponics-ESP1");
    NimBLEDevice::setPower(0);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityPasskey(pairingPasskey);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setMTU(247);
    server = NimBLEDevice::createServer();
    server->setCallbacks(&serverCallbacks);
    NimBLEService* service = server->createService(ServiceUuid);
    stateCharacteristic = service->createCharacteristic(
        StateUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_AUTHEN, 512);
    telemetryCharacteristic = service->createCharacteristic(
        TelemetryUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN, 256);
    telemetryCharacteristic->setCallbacks(&telemetryCallbacks);
    commandCharacteristic = service->createCharacteristic(
        CommandUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN, 256);
    commandCharacteristic->setCallbacks(&commandCallbacks);
    resultCharacteristic = service->createCharacteristic(
        ResultUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN, 256);
    NimBLECharacteristic* auth = service->createCharacteristic(
        AuthUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN, 32);
    auth->setValue("HYDRO_SECURE_V1");
    publishState();
    server->start();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName("Hydroponics-ESP1");
    advertising->addServiceUUID(ServiceUuid);
    advertising->enableScanResponse(true);
    advertising->start();
    Serial.println("BLE advertising started");
}

void pollButton() {
    static bool lastRaw = HIGH;
    static bool stable = HIGH;
    static unsigned long changed = 0;
    const bool raw = digitalRead(BootPin);
    if (raw != lastRaw) changed = millis();
    lastRaw = raw;
    if (raw != stable && millis() - changed > 50) {
        stable = raw;
        if (stable == LOW) setLowPower(!lowPower.load());
    }
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(600);
    pinMode(BootPin, INPUT_PULLUP);
    preferences.begin("hydro-demo", false);
    wifiSsid = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("password", "");
    ownerAddress = preferences.getString("owner", "");
    loadDevices();
    commandQueue = xQueueCreate(4, sizeof(CommandMessage*));
    if (!commandQueue) {
        Serial.println("Fatal: command queue allocation failed");
        while (true) delay(1000);
    }
    beginWifi();
    setupBle();
}

void loop() {
    pollButton();
    CommandMessage* message = nullptr;
    if (xQueueReceive(commandQueue, &message, 0) == pdTRUE && message) {
        processCommand(message->json);
        delete message;
    }
    const unsigned long now = millis();
    if (now - lastStateMs >= StateIntervalMs) {
        publishState();
        lastStateMs = now;
    }
    if (now - lastTelemetryMs >= TelemetryIntervalMs) {
        if (telemetrySubscribed.load()) publishTelemetry();
        lastTelemetryMs = now;
    }
    delay(10);
}

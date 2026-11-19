// Wi-Fi station shim over the Arduino WiFi library (C++),
// callable from C. Work package WP8 (Stage 8.4). Used only by the WifiMqtt
// task (transports/wifi_mqtt.c), which is the sole Wi-Fi owner.
//
// The Arduino libraries keep netif and the default event loop initialised
// exactly once (initialising them again from IDF calls would fail under
// arduino-esp32 2.0.x). Credentials stay in RAM: WiFi.persistent(false) sets
// WIFI_STORAGE_RAM, so the Wi-Fi driver never writes them to NVS.
// Compiled only with -D FW_ENABLE_WIFI.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wifiLanBegin(const char* hostname);  // station mode, RAM storage, no auto-reconnect
void wifiLanConnect(const char* ssid, const char* password);  // non-blocking
void wifiLanDisconnect(void);
bool wifiLanConnected(void);              // associated and has an IPv4 address
bool wifiLanIp(char out[16]);
int32_t wifiLanRssi(void);

// WP9 low power: WIFI_OFF (radio and its RF calibration stopped), and back to
// station mode. Both return false if the driver refused.
bool wifiLanRadioOff(void);
bool wifiLanRadioOn(void);

#ifdef __cplusplus
}
#endif

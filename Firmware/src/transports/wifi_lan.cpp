// Arduino WiFi implementation of wifi_lan.h. C++ only because the
// Arduino libraries are; everything it exports is C.
#ifdef FW_ENABLE_WIFI
#include "transports/wifi_lan.h"

#include <WiFi.h>
#include <stdio.h>

extern "C" bool wifiLanBegin(const char* hostname) {
  WiFi.persistent(false);         // WIFI_STORAGE_RAM: credentials never reach NVS from here
  WiFi.setAutoReconnect(false);   // retries are WifiMqtt's backoff, not the driver's
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.setHostname(hostname);
  // Modem sleep stays on (the default): required while BLE shares the radio.
  return true;
}

extern "C" void wifiLanConnect(const char* ssid, const char* password) {
  WiFi.disconnect(false, false);
  WiFi.begin(ssid, password);
}

extern "C" void wifiLanDisconnect(void) { WiFi.disconnect(false, false); }

extern "C" bool wifiLanConnected(void) {
  return WiFi.status() == WL_CONNECTED && static_cast<uint32_t>(WiFi.localIP()) != 0;
}

extern "C" bool wifiLanIp(char out[16]) {
  if (!wifiLanConnected()) {
    out[0] = '\0';
    return false;
  }
  const IPAddress ip = WiFi.localIP();
  snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return true;
}

extern "C" int32_t wifiLanRssi(void) { return wifiLanConnected() ? WiFi.RSSI() : 0; }

extern "C" bool wifiLanRadioOff(void) {
  WiFi.disconnect(true, false);  // wifioff = true
  return WiFi.mode(WIFI_OFF);
}

extern "C" bool wifiLanRadioOn(void) { return WiFi.mode(WIFI_STA); }

#endif  // FW_ENABLE_WIFI

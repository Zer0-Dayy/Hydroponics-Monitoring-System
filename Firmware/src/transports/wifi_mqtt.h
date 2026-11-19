// WifiMqtt: sole Wi-Fi owner (station), never restarts sensing. Work package
// WP8 (Stage 8.4). Architecture §17; FirmwareContract.md v2.
//
// Task "WifiMqtt" (cfgTaskSpec: core 0, priority 2, 6 KB):
//   station   credentials from Configuration; connect, and on failure retry
//             with exponential backoff 1, 2, 4 ... 60 s +/- 20 % jitter
//             (domain/backoff). A credential change or forget restarts it at
//             once. The Wi-Fi driver keeps nothing in NVS (WIFI_STORAGE_RAM).
//   status    state / SSID / IP / RSSI for the State frame (the app shows it
//             on its Wi-Fi page)
//   WP9       wifiMqttRequestStop() disconnects and puts the radio in
//             WIFI_OFF (low power); wifiMqttRadioOff() is the acknowledgement
//             PowerCoordinator waits for. RequestStart() turns it back on and
//             rejoins.
// The app talks to the controller over Bluetooth only: the read-only LAN link
// (TCP 47800 + mDNS) was removed after Stage 8.4. MQTT (server publishing) is
// not in scope yet; the name is kept from the architecture's task table.
//
// Built only with -D FW_ENABLE_WIFI (production envs). Without it the task
// does not exist and the status is always "unconfigured".
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/protocol_codec.h"  // PROTOCOL_SSID_MAX

typedef uint8_t WifiState;
enum { WIFI_STATE_UNCONFIGURED = 0, WIFI_STATE_CONNECTING, WIFI_STATE_CONNECTED, WIFI_STATE_FAILED };

typedef struct WifiStatus {
  WifiState state;
  char ssid[PROTOCOL_SSID_MAX + 1];
  char ip[16];             // dotted IPv4 when connected, else ""
  int32_t rssi;            // dBm when connected
} WifiStatus;

bool wifiMqttInit(void);          // starts the task; never waits for an access point
void wifiMqttRequestStop(void);   // WP9 power transition: drop Wi-Fi, radio off, preempt retry waits
void wifiMqttRequestStart(void);
bool wifiMqttRadioOff(void);      // true once a stop request has the radio in WIFI_OFF

void wifiMqttGetStatus(WifiStatus* out);  // zero wait, any task
const char* wifiStateName(WifiState s);   // contract spelling

typedef struct WifiMqttStats {
  uint32_t connects;          // station connections established
  uint32_t connectFailures;   // attempts that timed out or were refused
  uint32_t lastBackoffMs;
} WifiMqttStats;

void wifiMqttGetStats(WifiMqttStats* out);

// For Supervisor. NULL before init or without FW_ENABLE_WIFI.
void* wifiMqttTaskHandle(void);

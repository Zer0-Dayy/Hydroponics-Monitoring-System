#include "transports/frames.h"

#include <ctype.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#include "domain/protocol_codec.h"
#include "domain/sensor_set.h"
#include "hal/clock.h"
#include "policy_config.h"
#include "services/acquisition.h"
#include "services/power_coordinator.h"
#include "services/rail_state.h"
#include "services/safety.h"
#include "services/snapshot_store.h"
#include "transports/protocol.h"
#include "transports/wifi_mqtt.h"

static portMUX_TYPE s_bootMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_boot;
static bool s_bootSet;

uint32_t transportBootId(void) {
  const uint32_t candidate = esp_random();  // drawn outside the critical section
  portENTER_CRITICAL(&s_bootMux);
  if (!s_bootSet) {
    s_boot = candidate;
    s_bootSet = true;
  }
  const uint32_t boot = s_boot;
  portEXIT_CRITICAL(&s_bootMux);
  return boot;
}

size_t transportEncodeState(TransportScratch* s, const char* link, char* buf, size_t cap) {
  char mode[24];
  const char* name = powerModeName(powerModeCurrent());
  size_t i = 0;
  for (; name[i] != '\0' && i < sizeof(mode) - 1; ++i) mode[i] = (char)tolower((unsigned char)name[i]);
  mode[i] = '\0';

  SafetyStatus st;
  memset(&st, 0, sizeof(st));
  const bool haveSafety = safetyStatusPeek(&st);
  configurationPeekApplied(&s->cfg);
  WifiStatus wifi;
  wifiMqttGetStatus(&wifi);
  memcpy(s->ssid, wifi.ssid, sizeof(s->ssid));
  memcpy(s->ip, wifi.ip, sizeof(s->ip));
  const bool connected = wifi.state == WIFI_STATE_CONNECTED;
  const StateView v = {
      .node = TRANSPORT_NODE_NAME, .fw = PROTOCOL_FIRMWARE_VERSION, .boot = transportBootId(),
      .uptimeMs = monoNowMs(), .link = link, .mode = mode,
      .powerEpoch = railStateRead().epoch, .cfgRev = s->cfg.revision,  // live 5 V epoch (WP9)
      .wifiState = wifiStateName(wifi.state), .wifiSsid = s->ssid[0] != '\0' ? s->ssid : NULL,
      .wifiIp = connected ? s->ip : NULL, .wifiRssi = wifi.rssi,
      .relayOn = st.relayOn, .lampReason = haveSafety ? lampReasonName(st.lampReason) : "no data",
      .interlock = st.interlock, .latchedMask = st.latchedMask, .activeMask = st.activeMask,
      .staleMask = st.staleMask, .ruleCount = POLICY_RULE_COUNT,
      .sgp30Baseline = acquisitionSgp30BaselineName(), .enabled = s->cfg.sensorEnabled,
  };
  return protocolEncodeState(&v, buf, cap);
}

size_t transportEncodeTelemetry(TransportScratch* s, char* buf, size_t cap) {
  if (!snapshotPeek(&s->snap)) return 0;
  const RailState rail = railStateRead();
  snapshotGateRail(&s->snap, rail.valid5v, rail.epoch);  // the rail may have failed since acquisition
  configurationPeekApplied(&s->cfg);
  return protocolEncodeTelemetry(&s->snap, monoNowMs(), transportBootId(), s->cfg.revision, s->cfg.sensorEnabled,
                                 buf, cap);
}

size_t transportEncodeEvent(const JournalEvent* e, bool gap, char* buf, size_t cap) {
  const MonoMs now = monoNowMs();
  const EventView v = {e->n, e->ruleId, severityName(e->severity), journalKindName(e->kind),
                       now > e->atMs ? (uint32_t)(now - e->atMs) : 0u, gap};
  return protocolEncodeEvent(&v, buf, cap);
}

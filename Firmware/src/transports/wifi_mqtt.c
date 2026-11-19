#include "transports/wifi_mqtt.h"

#include <freertos/FreeRTOS.h>
#include <string.h>

static portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;
static WifiStatus s_status;  // all zero = unconfigured

const char* wifiStateName(WifiState s) {
  switch (s) {
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_FAILED: return "failed";
    default: return "unconfigured";
  }
}

void wifiMqttGetStatus(WifiStatus* out) {
  portENTER_CRITICAL(&s_statusMux);
  *out = s_status;
  portEXIT_CRITICAL(&s_statusMux);
}

#ifndef FW_ENABLE_WIFI

bool wifiMqttInit(void) { return false; }
void wifiMqttRequestStop(void) {}
void wifiMqttRequestStart(void) {}
bool wifiMqttRadioOff(void) { return true; }  // no Wi-Fi in this build: nothing to switch off
void wifiMqttGetStats(WifiMqttStats* out) { memset(out, 0, sizeof(*out)); }
void* wifiMqttTaskHandle(void) { return NULL; }

#else

#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>
#include <stdio.h>

#include "app_config.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "domain/backoff.h"
#include "hal/clock.h"
#include "services/configuration.h"
#include "transports/wifi_lan.h"

#define WIFI_HOSTNAME "hydro-esp1"
#define WIFI_WAKE_MS 50u
#define WIFI_ATTEMPT_TIMEOUT_MS 15000u  // one association + DHCP attempt
#define WIFI_JITTER_PCT 20u
#define WIFI_STATUS_REFRESH_MS 2000u    // RSSI / IP copy for State
#define WIFI_STATS_LOG_MS 60000u

typedef uint8_t Phase;
enum { PHASE_IDLE = 0, PHASE_ATTEMPT, PHASE_WAIT, PHASE_CONNECTED };

static StaticTask_t s_tcb;
static StackType_t s_stack[6 * 1024];  // cfgTaskSpec(TASK_ID_WIFI_MQTT)->stackBytes
static TaskHandle_t s_task = NULL;

static volatile bool s_stopRequested;
static volatile bool s_radioOff;  // WP9 acknowledgement: the radio is in WIFI_OFF
static portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
static WifiMqttStats s_stats;
#define STATS_ADD(field, n)          \
  do {                               \
    portENTER_CRITICAL(&s_statsMux); \
    s_stats.field += (n);            \
    portEXIT_CRITICAL(&s_statsMux);  \
  } while (0)

// Task-private (static: not on the 6 KB stack).
static ConfigWifi s_creds;
static uint32_t s_credsChanges = UINT32_MAX;
static Backoff s_backoff;
static Phase s_phase;
static MonoMs s_deadlineMs;

static void setStatus(WifiState state) {
  WifiStatus st;
  memset(&st, 0, sizeof(st));
  st.state = state;
  memcpy(st.ssid, s_creds.ssid, sizeof(st.ssid));
  if (state == WIFI_STATE_CONNECTED) {
    wifiLanIp(st.ip);
    st.rssi = wifiLanRssi();
  }
  portENTER_CRITICAL(&s_statusMux);
  s_status = st;
  portEXIT_CRITICAL(&s_statusMux);
}

static WifiState currentState(void) {
  portENTER_CRITICAL(&s_statusMux);
  const WifiState state = s_status.state;
  portEXIT_CRITICAL(&s_statusMux);
  return state;
}

// ---- Station ------------------------------------------------------------------------------
static void teardown(void) { wifiLanDisconnect(); }

static void startAttempt(MonoMs now) {
  wifiLanConnect(s_creds.ssid, s_creds.password);
  s_deadlineMs = now + WIFI_ATTEMPT_TIMEOUT_MS;
  s_phase = PHASE_ATTEMPT;
}

static void scheduleRetry(MonoMs now) {
  const uint32_t delay = backoffNext(&s_backoff, esp_random());
  portENTER_CRITICAL(&s_statsMux);
  s_stats.lastBackoffMs = delay;
  portEXIT_CRITICAL(&s_statsMux);
  s_deadlineMs = now + delay;
  s_phase = PHASE_WAIT;
}

static void onConnected(void) {
  s_phase = PHASE_CONNECTED;
  backoffReset(&s_backoff);
  STATS_ADD(connects, 1);
  setStatus(WIFI_STATE_CONNECTED);
  logPrintf(LOG_LEVEL_INFO, "wifi: connected, %s (RSSI %ld dBm)", s_status.ip, (long)s_status.rssi);
}

// Credentials come from Configuration; a change takes effect at once.
static void followConfiguration(MonoMs now) {
  uint32_t changes;
  configurationGetWifi(&s_creds, &changes);
  if (changes != s_credsChanges) {
    s_credsChanges = changes;
    teardown();
    backoffReset(&s_backoff);
    if (s_creds.ssid[0] != '\0' && !s_stopRequested) {
      logPrintf(LOG_LEVEL_INFO, "wifi: joining \"%s\"", s_creds.ssid);
      startAttempt(now);
      setStatus(WIFI_STATE_CONNECTING);
    } else {
      s_phase = PHASE_IDLE;
      setStatus(WIFI_STATE_UNCONFIGURED);
    }
  }
}

static void logStats(void) {
  WifiMqttStats t;
  wifiMqttGetStats(&t);
  WifiStatus st;
  wifiMqttGetStatus(&st);
  logPrintf(LOG_LEVEL_INFO, "wifi: %s %s rssi %ld | joins %lu fails %lu backoff %lums", wifiStateName(st.state), st.ip,
            (long)st.rssi, (unsigned long)t.connects, (unsigned long)t.connectFailures, (unsigned long)t.lastBackoffMs);
}

static void wifiTask(void* arg) {
  (void)arg;
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "wifi: task watchdog add failed");
  MonoMs nextStatus = 0, nextStats = monoNowMs() + WIFI_STATS_LOG_MS;
  bool stopped = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_WAKE_MS));
    const MonoMs now = monoNowMs();

    if (s_stopRequested != stopped) {  // WP9 power transitions
      stopped = s_stopRequested;
      s_credsChanges = UINT32_MAX;     // re-evaluated below: idle when stopped, rejoin when started
      if (stopped) {
        teardown();
        s_radioOff = wifiLanRadioOff();  // the PowerCoord acknowledgement
        logPrintf(LOG_LEVEL_INFO, "wifi: stopped on request, radio %s", s_radioOff ? "off" : "OFF FAILED");
      } else {
        s_radioOff = false;
        if (!wifiLanRadioOn()) logPrintf(LOG_LEVEL_ERROR, "wifi: radio restart failed");
        logPrintf(LOG_LEVEL_INFO, "wifi: started on request");
      }
    }
    followConfiguration(now);

    switch (s_phase) {
      case PHASE_ATTEMPT:
        if (wifiLanConnected()) {
          onConnected();
        } else if (now >= s_deadlineMs) {
          wifiLanDisconnect();
          STATS_ADD(connectFailures, 1);
          scheduleRetry(now);
          setStatus(WIFI_STATE_FAILED);
          logPrintf(LOG_LEVEL_WARN, "wifi: join failed, retry in %lu ms", (unsigned long)s_stats.lastBackoffMs);
        }
        break;
      case PHASE_WAIT:
        if (now >= s_deadlineMs) {
          startAttempt(now);
          setStatus(WIFI_STATE_CONNECTING);
        }
        break;
      case PHASE_CONNECTED:
        if (!wifiLanConnected()) {
          logPrintf(LOG_LEVEL_WARN, "wifi: connection lost");
          teardown();
          scheduleRetry(now);  // from 1 s: the backoff was reset on connect
          setStatus(WIFI_STATE_CONNECTING);
          break;
        }
        if (now >= nextStatus) {  // RSSI, IP for State
          setStatus(WIFI_STATE_CONNECTED);
          nextStatus = now + WIFI_STATUS_REFRESH_MS;
        }
        break;
      default: break;
    }

    if (now >= nextStats && currentState() != WIFI_STATE_UNCONFIGURED) {
      logStats();
      nextStats = now + WIFI_STATS_LOG_MS;
    }
    progressMark(TASK_ID_WIFI_MQTT);
    esp_task_wdt_reset();
  }
}

bool wifiMqttInit(void) {
  if (s_task != NULL) return true;
  backoffInit(&s_backoff, CFG_MQTT_BACKOFF_INITIAL_MS, CFG_MQTT_BACKOFF_MAX_MS, WIFI_JITTER_PCT);
  if (!wifiLanBegin(WIFI_HOSTNAME)) {
    logPrintf(LOG_LEVEL_ERROR, "wifi: station mode failed");
    return false;
  }
  const TaskSpec* spec = cfgTaskSpec(TASK_ID_WIFI_MQTT);
  if (spec->stackBytes != sizeof(s_stack)) {
    logPrintf(LOG_LEVEL_WARN, "wifi: stack %u B, task table says %lu B", (unsigned)sizeof(s_stack),
              (unsigned long)spec->stackBytes);
  }
  s_task = xTaskCreateStaticPinnedToCore(wifiTask, spec->name, sizeof(s_stack), NULL, spec->priority, s_stack, &s_tcb,
                                         spec->core);
  if (s_task == NULL) return false;
  logPrintf(LOG_LEVEL_INFO, "wifi: started (core %u, prio %u)", (unsigned)spec->core, (unsigned)spec->priority);
  return true;
}

void wifiMqttRequestStop(void) {
  s_stopRequested = true;
  if (s_task != NULL) xTaskNotifyGive(s_task);
}

void wifiMqttRequestStart(void) {
  s_stopRequested = false;
  if (s_task != NULL) xTaskNotifyGive(s_task);
}

bool wifiMqttRadioOff(void) { return s_task == NULL || (s_stopRequested && s_radioOff); }

void wifiMqttGetStats(WifiMqttStats* out) {
  portENTER_CRITICAL(&s_statsMux);
  *out = s_stats;
  portEXIT_CRITICAL(&s_statsMux);
}

void* wifiMqttTaskHandle(void) { return s_task; }

#endif  // FW_ENABLE_WIFI

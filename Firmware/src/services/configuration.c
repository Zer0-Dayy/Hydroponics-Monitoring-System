#include "services/configuration.h"

#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "domain/safety_core.h"
#include "hal/clock.h"
#include "policy_config.h"
#include "services/acquisition.h"
#include "services/flash_gate.h"
#include "services/safety.h"

#define CONFIG_NAMESPACE "hydro"
#define CONFIG_WAKE_MS 500u                 // idle wake: housekeeping, progress
#define CONFIG_ACK_TIMEOUT_MS 3000u         // acquisition applies at a frame boundary (<= 1 s)
#define CONFIG_ACK_POLL_MS 50u
#define CONFIG_GATE_WAIT_MS 100u            // one HC-SR04 ping is <= ~35 ms
#define CONFIG_GATE_TRIES 5
#define CONFIG_BASELINE_MIN_INTERVAL_MS (60u * 60u * 1000u)  // §16: rate-limited
#define CONFIG_RESET_HISTORY_AFTER_MS 30000u // not during a reset loop (unstable supply)
#define CONFIG_RESET_HISTORY_LEN 8

static const char* const kSlotKey[2] = {"cfg_a", "cfg_b"};

// ---- Mailboxes and queues (static) --------------------------------------------
static StaticQueue_t s_appliedControl;
static uint8_t s_appliedStorage[sizeof(ConfigBlob)];
static QueueHandle_t s_applied = NULL;

static StaticQueue_t s_requestControl;
static uint8_t s_requestStorage[CFG_COMMAND_QUEUE_LEN * sizeof(ConfigRequest)];
static QueueHandle_t s_requests = NULL;

static StaticTask_t s_tcb;
static StackType_t s_stack[4 * 1024];  // cfgTaskSpec(TASK_ID_CONFIGURATION)->stackBytes
static TaskHandle_t s_task = NULL;

// ---- State --------------------------------------------------------------------
static nvs_handle_t s_nvs;
static bool s_nvsOk;
static ConfigBlob s_current;   // task-owned after init
static uint8_t s_currentSlot;  // slot holding s_current (the other one is written next)
static bool s_haveSlot;        // s_current is stored somewhere

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_ackRevision;
static ConfigWifi s_wifi;
static uint32_t s_wifiChanges;
static bool s_haveStoredBaseline;
static uint16_t s_storedCo2, s_storedTvoc;
static bool s_offerPending;
static uint16_t s_offerCo2, s_offerTvoc;

static MonoMs s_lastBaselineWriteMs;
static bool s_baselineWritten;
static bool s_historyWritten;
static ConfigRequest s_req;  // task-private: keeps the stack small

// ---- Compiled defaults ---------------------------------------------------------
static void compiledDefaults(ConfigDefaults* d) {
  const LampControlConfig l = {POLICY_LAMP_ON_BELOW_MV, POLICY_LAMP_OFF_ABOVE_MV, POLICY_LAMP_MIN_ON_MS,
                               POLICY_LAMP_MIN_OFF_MS, POLICY_LAMP_FAULTS_TO_OFF};
  memset(d, 0, sizeof(*d));
  d->lampAutoEnabled = POLICY_LAMP_AUTO_ENABLED != 0;
  d->lamp = l;
  // pH: nominal fit from policy_config (published UNCALIBRATED as provisional).
  d->ph.slope = POLICY_PH_NOMINAL_PH_PER_MV;
  d->ph.offset = 7.0f - POLICY_PH_NOMINAL_PH_PER_MV * POLICY_PH_NOMINAL_MV_AT_PH7;
  d->ph.revision = 0;
  d->ph.valid = true;
  // TDS: nominal datasheet curve, K = 1 (D13), not calibrated.
  d->tds.slope = 1.0f;
  d->tds.offset = 0.0f;
  d->tds.revision = 0;
  d->tds.valid = false;
  // Light: no lux mapping until a reference-meter fit; published as mV.
  d->light.slope = 1.0f;
  d->light.offset = 0.0f;
  d->light.revision = 0;
  d->light.valid = false;
}

static void defaultBlob(ConfigBlob* b) {
  ConfigDefaults d;
  compiledDefaults(&d);
  configBlobInit(b, &d);
}

// ---- NVS helpers (every write goes through the flash gate) ---------------------
static void keepAlive(void) {
  if (s_task != NULL && xTaskGetCurrentTaskHandle() == s_task) {
    progressMark(TASK_ID_CONFIGURATION);
    esp_task_wdt_reset();
  }
}

static bool gatedSetBlob(const char* key, const void* data, size_t len) {
  if (!s_nvsOk) return false;
  for (int tries = 0; tries < CONFIG_GATE_TRIES; ++tries) {
    if (!flashGateTakeForWrite(CONFIG_GATE_WAIT_MS)) {
      keepAlive();
      continue;
    }
    esp_err_t err = data != NULL ? nvs_set_blob(s_nvs, key, data, len) : nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && data == NULL) err = ESP_OK;  // already gone
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    flashGateGiveAfterWrite();
    if (err != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "cfg: NVS write %s failed (%d)", key, (int)err);
    return err == ESP_OK;
  }
  logPrintf(LOG_LEVEL_WARN, "cfg: flash gate busy, %s not written", key);
  return false;
}

static bool getBlob(const char* key, void* out, size_t len) {
  if (!s_nvsOk) return false;
  size_t size = len;
  return nvs_get_blob(s_nvs, key, out, &size) == ESP_OK && size == len;
}

// A schema 1 slot (firmware up to Stage 8.4) as a schema 2 blob, same revision.
static bool loadV1(const char* key, const ConfigDefaults* d, ConfigBlob* out) {
  ConfigBlobV1 old;
  if (!getBlob(key, &old, sizeof(old))) return false;
  if (!configBlobMigrateV1(&old, sizeof(old), d, out)) return false;
  logPrintf(LOG_LEVEL_INFO, "cfg: %s migrated from schema 1 (rev %u)", key, (unsigned)out->revision);
  return true;
}

// Writes the slot not holding the current config; on success it becomes current.
static bool storeConfig(const ConfigBlob* next) {
  const uint8_t slot = s_haveSlot ? (uint8_t)(1u - s_currentSlot) : 0u;
  if (!gatedSetBlob(kSlotKey[slot], next, sizeof(*next))) return false;
  memcpy(&s_current, next, sizeof(s_current));
  s_currentSlot = slot;
  s_haveSlot = true;
  return true;
}

static void publish(void) { xQueueOverwrite(s_applied, &s_current); }

// ---- Requests -------------------------------------------------------------------
static void reply(const ConfigRequest* req, ProtocolError error, uint16_t cfgRev) {
  if (req->reply == NULL) return;
  ConfigResult r;
  memset(&r, 0, sizeof(r));
  r.tag = req->tag;
  r.error = error;
  r.cfgRev = cfgRev;
  req->reply(&r, req->replyCtx);
}

static uint16_t ackedRevision(void) {
  portENTER_CRITICAL(&s_mux);
  const uint16_t rev = s_ackRevision;
  portEXIT_CRITICAL(&s_mux);
  return rev;
}

static bool waitApplied(uint16_t revision) {
  if (acquisitionTaskHandle() == NULL) return false;
  xTaskNotifyGive(acquisitionTaskHandle());  // apply at the next frame boundary
  for (uint32_t waited = 0; waited < CONFIG_ACK_TIMEOUT_MS; waited += CONFIG_ACK_POLL_MS) {
    if (ackedRevision() == revision) return true;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ACK_POLL_MS));
    keepAlive();  // the wait is normal work, not a stall
  }
  return ackedRevision() == revision;
}

static void handleSetSensor(const ConfigRequest* req) {
  if (req->sensor >= SENSOR_COUNT) {
    reply(req, PROTOCOL_ERR_BAD_FIELD, s_current.revision);
    return;
  }
  if (!req->enabled) {
    // Switching an input off must never silence an alarm (§12). The status can
    // be up to one Safety pass (100 ms) old; a TRIP in flight still latches,
    // and a latched rule stays visible as stale (safety_core), so the race is
    // fail-safe. Accepted gap: a rule that has not tripped yet cannot trip
    // once its sensor is off.
    SafetyStatus st;
    memset(&st, 0, sizeof(st));
    if (safetyStatusPeek(&st) &&
        safetyRulesBlockSensor(kPolicyRules, POLICY_RULE_COUNT, st.latchedMask, st.activeMask, req->sensor)) {
      reply(req, PROTOCOL_ERR_RULE_LATCHED, s_current.revision);
      return;
    }
  }
  ConfigBlob next;
  if (!configBlobWithSensor(&s_current, req->sensor, req->enabled, &next)) {
    reply(req, PROTOCOL_OK, s_current.revision);  // already so
    return;
  }
  if (!storeConfig(&next)) {
    reply(req, PROTOCOL_ERR_STORAGE, s_current.revision);
    return;
  }
  publish();
  logPrintf(LOG_LEVEL_INFO, "cfg: rev %u: %s %s", (unsigned)s_current.revision, sensorKey(req->sensor),
            req->enabled ? "enabled" : "disabled");
  const bool applied = waitApplied(s_current.revision);
  if (!applied) logPrintf(LOG_LEVEL_WARN, "cfg: rev %u stored but not applied in time", (unsigned)s_current.revision);
  reply(req, applied ? PROTOCOL_OK : PROTOCOL_ERR_TIMEOUT, s_current.revision);
}

static void handleWifi(const ConfigRequest* req, bool forget) {
  ConfigWifi wifi;
  memset(&wifi, 0, sizeof(wifi));
  if (!forget) {
    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", req->ssid);
    snprintf(wifi.password, sizeof(wifi.password), "%s", req->password);
  }
  const bool ok = gatedSetBlob("wifi", forget ? NULL : &wifi, sizeof(wifi));
  if (ok) {
    portENTER_CRITICAL(&s_mux);
    memcpy(&s_wifi, &wifi, sizeof(s_wifi));
    ++s_wifiChanges;
    portEXIT_CRITICAL(&s_mux);
  }
  memset(&wifi, 0, sizeof(wifi));  // credentials never linger on the stack
  logPrintf(LOG_LEVEL_INFO, "cfg: wifi %s %s", forget ? "forget" : "set", ok ? "stored" : "FAILED");
  reply(req, ok ? PROTOCOL_OK : PROTOCOL_ERR_STORAGE, s_current.revision);
}

// A calibration must never hide an alarm the sensor is part of (§12), as for
// switching it off: refused while one of its rules is latched or active.
static ProtocolError calibrationBlocked(SensorId sensor) {
  SafetyStatus st;
  memset(&st, 0, sizeof(st));
  if (!safetyStatusPeek(&st)) return PROTOCOL_OK;
  if (safetyRulesBlockSensor(kPolicyRules, POLICY_RULE_COUNT, st.latchedMask, 0, sensor)) {
    return PROTOCOL_ERR_RULE_LATCHED;
  }
  if (safetyRulesBlockSensor(kPolicyRules, POLICY_RULE_COUNT, 0, st.activeMask, sensor)) {
    return PROTOCOL_ERR_RULE_ACTIVE;
  }
  return PROTOCOL_OK;
}

static bool fitCalibration(const ConfigRequest* req, LinearCalibration* out) {
  const float x1 = (float)req->calX1 / CAL_X_SCALE;
  switch (req->sensor) {
    case SENSOR_PH:
      return phFitFromBuffers(x1, (float)req->calY1 / CAL_PH_Y_SCALE, (float)req->calX2 / CAL_X_SCALE,
                              (float)req->calY2 / CAL_PH_Y_SCALE, POLICY_PH_SLOPE_MIN_ABS, POLICY_PH_SLOPE_MAX_ABS,
                              0, out);
    case SENSOR_TDS:
      return linearGainFromPoint(x1, (float)req->calY1 / CAL_Y_SCALE, POLICY_TDS_K_MIN, POLICY_TDS_K_MAX, 0, out);
    case SENSOR_LIGHT:
      return linearGainFromPoint(x1, (float)req->calY1 / CAL_Y_SCALE, POLICY_LIGHT_LUX_PER_MV_MIN,
                                 POLICY_LIGHT_LUX_PER_MV_MAX, 0, out);
    default: return false;
  }
}

static void handleCalibration(const ConfigRequest* req, bool reset) {
  if (configCalibrationBit(req->sensor) == 0) {
    reply(req, PROTOCOL_ERR_BAD_FIELD, s_current.revision);
    return;
  }
  const ProtocolError blocked = calibrationBlocked(req->sensor);
  if (blocked != PROTOCOL_OK) {
    reply(req, blocked, s_current.revision);
    return;
  }
  LinearCalibration cal;
  if (reset) {
    ConfigDefaults d;
    compiledDefaults(&d);
    cal = req->sensor == SENSOR_PH ? d.ph : req->sensor == SENSOR_TDS ? d.tds : d.light;
  } else if (!fitCalibration(req, &cal)) {
    reply(req, PROTOCOL_ERR_BAD_FIT, s_current.revision);
    return;
  }
  ConfigBlob next;
  configBlobWithCalibration(&s_current, req->sensor, &cal, !reset, &next);
  if (!storeConfig(&next)) {
    reply(req, PROTOCOL_ERR_STORAGE, s_current.revision);
    return;
  }
  publish();
  if (reset) {
    logPrintf(LOG_LEVEL_INFO, "cfg: rev %u: %s calibration reset to default", (unsigned)s_current.revision,
              sensorKey(req->sensor));
  } else {
    logPrintf(LOG_LEVEL_INFO, "cfg: rev %u: %s calibrated, slope %.6f offset %.4f", (unsigned)s_current.revision,
              sensorKey(req->sensor), (double)cal.slope, (double)cal.offset);
  }
  const bool applied = waitApplied(s_current.revision);
  if (!applied) logPrintf(LOG_LEVEL_WARN, "cfg: rev %u stored but not applied in time", (unsigned)s_current.revision);
  reply(req, applied ? PROTOCOL_OK : PROTOCOL_ERR_TIMEOUT, s_current.revision);
}

static void handle(const ConfigRequest* req) {
  // Stale requests never change flash (§11): wrong session = no reply at all.
  if (req->sessionNow != NULL && req->sessionNow() != req->session) return;
  if (monoNowMs() > req->expiresMs) {
    reply(req, PROTOCOL_ERR_EXPIRED, s_current.revision);
    return;
  }
  switch (req->kind) {
    case CONFIG_REQ_SET_SENSOR_ENABLED: handleSetSensor(req); break;
    case CONFIG_REQ_SET_WIFI: handleWifi(req, false); break;
    case CONFIG_REQ_FORGET_WIFI: handleWifi(req, true); break;
    case CONFIG_REQ_SET_CALIBRATION: handleCalibration(req, false); break;
    case CONFIG_REQ_RESET_CALIBRATION: handleCalibration(req, true); break;
    default: reply(req, PROTOCOL_ERR_UNKNOWN_OP, s_current.revision); break;
  }
}

// ---- Housekeeping ---------------------------------------------------------------
static void maybeWriteBaseline(MonoMs now) {
  portENTER_CRITICAL(&s_mux);
  const bool pending = s_offerPending;
  const uint16_t co2 = s_offerCo2, tvoc = s_offerTvoc;
  s_offerPending = false;
  portEXIT_CRITICAL(&s_mux);
  if (!pending) return;
  if (s_haveStoredBaseline && co2 == s_storedCo2 && tvoc == s_storedTvoc) return;
  if (s_baselineWritten && now - s_lastBaselineWriteMs < CONFIG_BASELINE_MIN_INTERVAL_MS) return;
  const uint16_t pair[2] = {co2, tvoc};
  if (gatedSetBlob("sgp_base", pair, sizeof(pair))) {
    s_storedCo2 = co2;
    s_storedTvoc = tvoc;
    s_haveStoredBaseline = true;
    s_baselineWritten = true;
    s_lastBaselineWriteMs = now;
    logPrintf(LOG_LEVEL_INFO, "cfg: SGP30 baseline stored (co2eq 0x%04x, tvoc 0x%04x)", co2, tvoc);
  }
}

static void maybeWriteResetHistory(MonoMs now) {
  if (s_historyWritten || now < CONFIG_RESET_HISTORY_AFTER_MS) return;
  s_historyWritten = true;
  uint8_t hist[CONFIG_RESET_HISTORY_LEN];
  if (!getBlob("rst_hist", hist, sizeof(hist))) memset(hist, 0, sizeof(hist));  // 0 = ESP_RST_UNKNOWN
  memmove(&hist[1], &hist[0], sizeof(hist) - 1);
  hist[0] = (uint8_t)esp_reset_reason();
  if (gatedSetBlob("rst_hist", hist, sizeof(hist))) {
    logPrintf(LOG_LEVEL_INFO, "cfg: reset history (newest first, esp_reset_reason_t): %u %u %u %u %u %u %u %u",
              hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
  }
}

static void configTask(void* arg) {
  (void)arg;
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "cfg: task watchdog add failed");
  for (;;) {
    if (xQueueReceive(s_requests, &s_req, pdMS_TO_TICKS(CONFIG_WAKE_MS)) == pdTRUE) {
      handle(&s_req);
      memset(s_req.password, 0, sizeof(s_req.password));
    }
    const MonoMs now = monoNowMs();
    maybeWriteBaseline(now);
    maybeWriteResetHistory(now);
    progressMark(TASK_ID_CONFIGURATION);
    esp_task_wdt_reset();
  }
}

// ---- Public API --------------------------------------------------------------------
bool configurationInit(void) {
  if (s_task != NULL) return true;
  s_applied = xQueueCreateStatic(1, sizeof(ConfigBlob), s_appliedStorage, &s_appliedControl);
  s_requests = xQueueCreateStatic(CFG_COMMAND_QUEUE_LEN, sizeof(ConfigRequest), s_requestStorage,
                                  &s_requestControl);
  if (s_applied == NULL || s_requests == NULL) return false;

  // NVS itself was initialised by the Arduino core (initArduino()).
  s_nvsOk = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &s_nvs) == ESP_OK;
  if (!s_nvsOk) logPrintf(LOG_LEVEL_ERROR, "cfg: NVS open failed - compiled defaults, nothing stored");

  ConfigDefaults defaults;
  compiledDefaults(&defaults);
  ConfigBlob slots[2];
  bool have[2];
  for (uint8_t i = 0; i < 2; ++i) {
    have[i] = getBlob(kSlotKey[i], &slots[i], sizeof(ConfigBlob)) || loadV1(kSlotKey[i], &defaults, &slots[i]);
  }
  const ConfigBlob* newest = configBlobPickNewest(have[0] ? &slots[0] : NULL, have[1] ? &slots[1] : NULL);
  bool store = false;
  if (newest != NULL) {
    memcpy(&s_current, newest, sizeof(s_current));
    s_currentSlot = newest == &slots[0] ? 0 : 1;
    s_haveSlot = true;
  } else {
    defaultBlob(&s_current);
    store = s_nvsOk;  // first boot or both slots corrupt
    if (have[0] || have[1]) logPrintf(LOG_LEVEL_WARN, "cfg: stored config invalid - defaults");
  }
  if (configBlobRefreshDefaults(&s_current, &defaults)) store = s_nvsOk;
  if (store) {
    ConfigBlob next;
    memcpy(&next, &s_current, sizeof(next));
    if (!storeConfig(&next)) logPrintf(LOG_LEVEL_WARN, "cfg: could not store rev %u", (unsigned)next.revision);
  }
  s_ackRevision = 0;
  publish();  // before Safety and acquisition start (they peek it on their first pass)

  ConfigWifi wifi;
  if (getBlob("wifi", &wifi, sizeof(wifi))) {
    wifi.ssid[PROTOCOL_SSID_MAX] = '\0';
    wifi.password[PROTOCOL_PASSWORD_MAX] = '\0';
    memcpy(&s_wifi, &wifi, sizeof(s_wifi));
    memset(&wifi, 0, sizeof(wifi));
  }
  size_t tokenSize = 0;  // LAN link removed: drop its token once
  if (s_nvsOk && nvs_get_blob(s_nvs, "lan_tok", NULL, &tokenSize) == ESP_OK) {
    gatedSetBlob("lan_tok", NULL, 0);
    logPrintf(LOG_LEVEL_INFO, "cfg: old LAN token erased");
  }
  // SGP30 baseline (Sensirion integration guide §3.8): valid only if the
  // sensor was off for less than 7 days. There is no wall clock, so the
  // downtime is known only for resets where the chip kept its supply (software,
  // panic, watchdog, reset pin: seconds). After a power-on, brown-out or
  // unknown reset it may have been off any time: relearn, and erase the old
  // baseline so a later warm reset cannot bring it back.
  uint16_t pair[2];
  if (getBlob("sgp_base", pair, sizeof(pair))) {
    const esp_reset_reason_t why = esp_reset_reason();
    const bool supplyKept = why == ESP_RST_SW || why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                            why == ESP_RST_TASK_WDT || why == ESP_RST_WDT || why == ESP_RST_EXT;
    if (supplyKept) {
      s_storedCo2 = pair[0];
      s_storedTvoc = pair[1];
      s_haveStoredBaseline = true;
    } else {
      gatedSetBlob("sgp_base", NULL, 0);
      logPrintf(LOG_LEVEL_INFO, "cfg: SGP30 baseline dropped (reset %d: off-time unknown, relearning)", (int)why);
    }
  }

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_CONFIGURATION);
  if (spec->stackBytes != sizeof(s_stack)) {
    logPrintf(LOG_LEVEL_WARN, "cfg: stack %u B, task table says %lu B", (unsigned)sizeof(s_stack),
              (unsigned long)spec->stackBytes);
  }
  s_task = xTaskCreateStaticPinnedToCore(configTask, spec->name, sizeof(s_stack), NULL, spec->priority,
                                         s_stack, &s_tcb, spec->core);
  logPrintf(LOG_LEVEL_INFO, "cfg: rev %u, sensors 0x%02x, calibrated 0x%02x, wifi %s, SGP30 baseline %s",
            (unsigned)s_current.revision, (unsigned)s_current.sensorEnabled, (unsigned)s_current.calOverridden,
            s_wifi.ssid[0] != '\0' ? "set" : "none", s_haveStoredBaseline ? "stored" : "none");
  return s_task != NULL;
}

bool configurationSubmit(const ConfigRequest* request) {
  if (s_requests == NULL || request == NULL) return false;
  return xQueueSend(s_requests, request, 0) == pdTRUE;
}

bool configurationPeekApplied(ConfigBlob* out) {
  if (s_applied != NULL && xQueuePeek(s_applied, out, 0) == pdTRUE) return true;
  defaultBlob(out);
  return false;
}

bool configurationStoredBaseline(uint16_t* co2eq, uint16_t* tvoc) {
  if (!s_haveStoredBaseline) return false;
  *co2eq = s_storedCo2;
  *tvoc = s_storedTvoc;
  return true;
}

void configurationOfferBaseline(uint16_t co2eq, uint16_t tvoc) {
  portENTER_CRITICAL(&s_mux);
  s_offerPending = true;
  s_offerCo2 = co2eq;
  s_offerTvoc = tvoc;
  portEXIT_CRITICAL(&s_mux);
}

void configurationAckApplied(uint16_t revision) {
  portENTER_CRITICAL(&s_mux);
  s_ackRevision = revision;
  portEXIT_CRITICAL(&s_mux);
}

bool configurationGetWifi(ConfigWifi* out, uint32_t* changeCount) {
  portENTER_CRITICAL(&s_mux);
  memcpy(out, &s_wifi, sizeof(*out));
  if (changeCount != NULL) *changeCount = s_wifiChanges;
  portEXIT_CRITICAL(&s_mux);
  return out->ssid[0] != '\0';
}

void* configurationTaskHandle(void) { return s_task; }

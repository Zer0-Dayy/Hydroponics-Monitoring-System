#include "services/safety.h"

#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "app_config.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "hal/clock.h"
#include "hal/relay.h"
#include "domain/alarm_sticky.h"
#include "domain/sensor_set.h"
#include "policy_config.h"
#include "services/configuration.h"
#include "services/rail_state.h"
#include "services/snapshot_store.h"

_Static_assert(EVENT_JOURNAL_LEN == CFG_ALARM_JOURNAL_LEN, "journal length must match app_config.h");

// ---- Sticky per-rule store (§11): survives a full detail queue -------------
// Protocol in domain/alarm_sticky.h; every call is under s_stickyMux.
static portMUX_TYPE s_stickyMux = portMUX_INITIALIZER_UNLOCKED;
static StickySlot s_sticky[POLICY_RULE_COUNT];
static volatile uint32_t s_detailOverflow;  // single writer per increment site, read for status only

// ---- Queues and task (all static, §16) --------------------------------------
static StaticQueue_t s_detailControl;
static uint8_t s_detailStorage[CFG_ALARM_DETAIL_QUEUE_LEN * sizeof(AlarmEvent)];
static QueueHandle_t s_detail = NULL;

static StaticQueue_t s_statusControl;
static uint8_t s_statusStorage[sizeof(SafetyStatus)];
static QueueHandle_t s_status = NULL;

static StaticTask_t s_tcb;
static StackType_t s_stack[3 * 1024];  // cfgTaskSpec(TASK_ID_SAFETY)->stackBytes
static TaskHandle_t s_task = NULL;

// ---- WP8: command queue and event journal -----------------------------------
typedef struct SafetyRequest {
  SafetyCommand command;
  uint32_t session;              // transport session generation at receive time
  uint32_t (*sessionNow)(void);  // NULL = no session check
  SafetyReplyFn reply;
  void* ctx;
} SafetyRequest;

static StaticQueue_t s_commandControl;
static uint8_t s_commandStorage[SAFETY_MAX_COMMANDS * sizeof(SafetyRequest)];
static QueueHandle_t s_commands = NULL;

static portMUX_TYPE s_journalMux = portMUX_INITIALIZER_UNLOCKED;
static EventJournal s_journal;

// Task-private state: static, never on the 3 KiB stack.
static SafetyRequest s_requests[SAFETY_MAX_COMMANDS];
static SafetyCommand s_cmds[SAFETY_MAX_COMMANDS];
static ConfigBlob s_cfg;
static SafetyCore s_core;
static Snapshot s_snap;
static AlarmEvent s_events[CFG_ALARM_DETAIL_QUEUE_LEN + 2 * POLICY_RULE_COUNT];
static SafetyStatus s_statusOut;

#ifdef FW_ALARM_LATENCY_TEST
// ---- Alarm latency test (env:alarm_latency, §19) -----------------------------
// submit -> action: esp_timer us from acquisition's submit to the end of the
//                   Safety pass that applied the decision (relay written).
// sample -> action: ms from the sample's intervalEndMs (BME280 read done).
static portMUX_TYPE s_latMux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_submitUs;  // 0 = no event waiting

typedef struct LatencyStats {
  uint32_t n;
  int64_t minUs, maxUs, sumUs;
  uint32_t minMs, maxMs, sumMs;
} LatencyStats;
static LatencyStats s_lat;
static uint32_t s_latReports;

void safetyLatencyMarkSubmit(void) {
  const int64_t t = esp_timer_get_time();
  portENTER_CRITICAL(&s_latMux);
  s_submitUs = t;
  portEXIT_CRITICAL(&s_latMux);
}

static void latencyRecord(const AlarmEvent* events, uint8_t n) {
  const AlarmEvent* trip = NULL;
  for (uint8_t i = 0; i < n; ++i) {
    if (events[i].decision == RULE_DECISION_TRIP) trip = &events[i];
  }
  portENTER_CRITICAL(&s_latMux);
  const int64_t submitted = s_submitUs;
  s_submitUs = 0;
  portEXIT_CRITICAL(&s_latMux);
  if (trip == NULL || submitted == 0) return;

  const int64_t us = esp_timer_get_time() - submitted;
  const MonoMs now = monoNowMs();
  const uint32_t ms = now > trip->sample.intervalEndMs ? (uint32_t)(now - trip->sample.intervalEndMs) : 0;
  if (ms > 5 || us > 5000) {  // one slow trip: when exactly (the 20-trip summary hides it)
    logPrintf(LOG_LEVEL_WARN, "latency SPIKE at t=%lu ms: submit->action %lld us, sample->action %lu ms",
              (unsigned long)now, (long long)us, (unsigned long)ms);
  }
  if (s_lat.n == 0 || us < s_lat.minUs) s_lat.minUs = us;
  if (s_lat.n == 0 || us > s_lat.maxUs) s_lat.maxUs = us;
  if (s_lat.n == 0 || ms < s_lat.minMs) s_lat.minMs = ms;
  if (s_lat.n == 0 || ms > s_lat.maxMs) s_lat.maxMs = ms;
  s_lat.sumUs += us;
  s_lat.sumMs += ms;
  if (++s_lat.n == 20) {
    logPrintf(LOG_LEVEL_INFO,
              "latency #%lu (20 TRIPs): submit->action us min %lld avg %lld max %lld | "
              "sample->action ms min %lu avg %lu max %lu",
              (unsigned long)++s_latReports, (long long)s_lat.minUs, (long long)(s_lat.sumUs / 20),
              (long long)s_lat.maxUs, (unsigned long)s_lat.minMs, (unsigned long)(s_lat.sumMs / 20),
              (unsigned long)s_lat.maxMs);
    memset(&s_lat, 0, sizeof(s_lat));
  }
}
#endif

bool safetySubmitEvent(const AlarmEvent* e) {
  if (e == NULL || e->ruleId >= POLICY_RULE_COUNT) return false;
  if (e->decision != RULE_DECISION_TRIP && e->decision != RULE_DECISION_CLEAR) return false;
  StickySlot* slot = &s_sticky[e->ruleId];

  portENTER_CRITICAL(&s_stickyMux);
  stickyBegin(slot, &kPolicyRules[e->ruleId], e);  // in flight: Safety leaves the slot alone
  portEXIT_CRITICAL(&s_stickyMux);

  const bool queued = s_detail != NULL && xQueueSend(s_detail, e, 0) == pdTRUE;  // never inside the portMUX
  portENTER_CRITICAL(&s_stickyMux);
  stickyEnd(slot, queued);
  if (!queued) ++s_detailOverflow;
  portEXIT_CRITICAL(&s_stickyMux);
  if (s_task != NULL) xTaskNotifyGive(s_task);  // after stickyEnd(): the next pass sees the outcome
  return queued;
}

// Drain the detail queue, then, only for a rule whose detail record was
// dropped, let its sticky slot re-supply what may be missing: the TRIP (if
// none came through the queue) and, if the rule's last decision was CLEAR,
// that CLEAR too, so the rule cannot stay active forever. A record that was
// queued arrives through the queue, so nothing is processed twice. A slot
// whose send is still in flight is collected on the next pass.
static uint8_t drainEvents(void) {
  uint8_t n = 0;
  while (n < CFG_ALARM_DETAIL_QUEUE_LEN && xQueueReceive(s_detail, &s_events[n], 0) == pdTRUE) ++n;

  bool sawTrip[POLICY_RULE_COUNT] = {false};
  for (uint8_t i = 0; i < n; ++i) {
    if (s_events[i].ruleId < POLICY_RULE_COUNT && s_events[i].decision == RULE_DECISION_TRIP) {
      sawTrip[s_events[i].ruleId] = true;
    }
  }
  portENTER_CRITICAL(&s_stickyMux);
  n = (uint8_t)(n + stickyCollect(s_sticky, POLICY_RULE_COUNT, sawTrip, &s_events[n]));
  portEXIT_CRITICAL(&s_stickyMux);
  return n;
}

bool safetySubmitCommand(const SafetyCommand* command, uint32_t session, uint32_t (*sessionNow)(void),
                         SafetyReplyFn reply, void* ctx) {
  if (s_commands == NULL || command == NULL) return false;
  const SafetyRequest request = {*command, session, sessionNow, reply, ctx};
  if (xQueueSend(s_commands, &request, 0) != pdTRUE) return false;
  if (s_task != NULL) xTaskNotifyGive(s_task);
  return true;
}

void safetyJournalCursorInit(JournalCursor* cursor) {
  portENTER_CRITICAL(&s_journalMux);
  journalCursorInit(&s_journal, cursor);
  portEXIT_CRITICAL(&s_journalMux);
}

bool safetyJournalNext(JournalCursor* cursor, JournalEvent* out, bool* gap) {
  portENTER_CRITICAL(&s_journalMux);
  const bool got = journalNext(&s_journal, cursor, out, gap);
  portEXIT_CRITICAL(&s_journalMux);
  return got;
}

static void journal(uint8_t ruleId, JournalKind kind, MonoMs atMs, float value) {
  JournalEvent e;
  memset(&e, 0, sizeof(e));
  e.ruleId = ruleId;
  e.severity = ruleId < POLICY_RULE_COUNT ? kPolicyRules[ruleId].severity : SEVERITY_INFO;
  e.kind = kind;
  e.atMs = atMs;
  e.value = value;
  portENTER_CRITICAL(&s_journalMux);
  journalAppend(&s_journal, &e);
  portEXIT_CRITICAL(&s_journalMux);
}

// A request from a link that has since closed (or reconnected) is dropped
// unapplied and unanswered, as Configuration does (§11).
static uint8_t drainCommands(void) {
  uint8_t n = 0;
  while (n < SAFETY_MAX_COMMANDS && xQueueReceive(s_commands, &s_requests[n], 0) == pdTRUE) {
    const SafetyRequest* r = &s_requests[n];
    if (r->sessionNow != NULL && r->sessionNow() != r->session) {
      logPrintf(LOG_LEVEL_INFO, "safety: rule %u reset from a closed session dropped", (unsigned)r->command.ruleId);
      continue;
    }
    s_cmds[n] = r->command;
    ++n;
  }
  return n;
}

static void safetyTask(void* arg) {
  (void)arg;
  SafetyOutputs out;
  bool relayOn = false;
  LampReason lastReason = LAMP_REASON_NO_DATA;
  uint32_t passes = 0;

  // Task watchdog (§18): reset by this task only, after each completed pass
  // (at least every 100 ms). Supervisor never resets it on Safety's behalf.
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "safety: task watchdog add failed");

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CFG_SAFETY_HEALTH_CHECK_MS));
#ifdef FW_INJECT_STALL_SAFETY
    // Acceptance test (§19 "Stall injection"): block forever after N ms.
    if (monoNowMs() >= (MonoMs)(FW_INJECT_STALL_SAFETY)) {
      logPrintf(LOG_LEVEL_WARN, "safety: INJECTED STALL - blocking forever");
      vTaskDelay(portMAX_DELAY);
    }
#endif
    const MonoMs now = monoNowMs();
    const uint8_t n = drainEvents();
    const uint8_t nCmd = drainCommands();
    const bool haveSnap = snapshotPeek(&s_snap);
    const RailState rail = railStateRead();
    if (haveSnap) snapshotGateRail(&s_snap, rail.valid5v, rail.epoch);  // no 5 V value of an older epoch
    configurationPeekApplied(&s_cfg);  // defaults (all on) if Configuration is not running
    const SensorMask disabled = (SensorMask)(SENSOR_MASK_ALL & (SensorMask)~s_cfg.sensorEnabled);
    const SafetyInputs in = {haveSnap ? &s_snap : NULL, s_events, n, now, s_cmds, nCmd, disabled,
                             !rail.valid5v};
    safetyCoreStep(&s_core, &in, &out);

    const bool relayChanged = out.relayOn != relayOn;
    if (relayChanged) {
      relayWrite(out.relayOn);
      relayOn = out.relayOn;
    }
#ifdef FW_ALARM_LATENCY_TEST
    latencyRecord(s_events, n);  // the action is applied: stop the clock
#endif
    if (relayChanged) {
      logPrintf(LOG_LEVEL_INFO, "safety: relay %s (%s)", relayOn ? "ON" : "OFF",
                lampReasonName(out.lampReason));
    } else if (out.lampReason != lastReason &&
               (out.lampReason == LAMP_REASON_STALE || out.lampReason == LAMP_REASON_FAULT ||
                out.lampReason == LAMP_REASON_INTERLOCK)) {
      logPrintf(LOG_LEVEL_WARN, "safety: lamp %s", lampReasonName(out.lampReason));
    }
    lastReason = out.lampReason;
    for (uint8_t i = 0; i < out.resultCount && i < nCmd; ++i) {
      if (out.results[i].outcome == SAFETY_CMD_APPLIED) {
        journal(s_cmds[i].ruleId, JOURNAL_KIND_RESET, now, 0.0f);
        logPrintf(LOG_LEVEL_INFO, "safety: rule %u latch reset", (unsigned)s_cmds[i].ruleId);
      }
      if (s_requests[i].reply != NULL) {
        s_requests[i].reply(out.results[i].tag, out.results[i].outcome, s_requests[i].ctx);
      }
    }
    for (uint8_t i = 0; i < n; ++i) {
#ifdef FW_ALARM_LATENCY_TEST
      if (s_events[i].decision == RULE_DECISION_TRIP && s_events[i].ruleId == 0) continue;  // 4 per s
#endif
      journal(s_events[i].ruleId,
              s_events[i].decision == RULE_DECISION_TRIP ? JOURNAL_KIND_TRIP : JOURNAL_KIND_CLEAR,
              s_events[i].sample.intervalEndMs, s_events[i].sample.value);
      logPrintf(LOG_LEVEL_WARN, "safety: rule %u %s (%s = %.2f)", (unsigned)s_events[i].ruleId,
                s_events[i].decision == RULE_DECISION_TRIP ? "TRIP" : "CLEAR",
                metricName(s_events[i].sample.metric), (double)s_events[i].sample.value);
    }

    s_statusOut.atMs = now;
    s_statusOut.relayOn = relayOn;
    s_statusOut.lampWantsOn = out.lampWantsOn;
    s_statusOut.lampReason = out.lampReason;
    s_statusOut.interlock = out.interlock;
    s_statusOut.latchedMask = out.latchedMask;
    s_statusOut.activeMask = out.activeMask;
    s_statusOut.staleMask = out.staleMask;
    s_statusOut.detailOverflow = s_detailOverflow;
    s_statusOut.passes = ++passes;
    xQueueOverwrite(s_status, &s_statusOut);
    progressMark(TASK_ID_SAFETY);
    esp_task_wdt_reset();
  }
}

TaskHandle_t safetyTaskHandle(void) { return s_task; }

bool safetyInit(void) {
  if (s_task != NULL) return true;
  s_detail = xQueueCreateStatic(CFG_ALARM_DETAIL_QUEUE_LEN, sizeof(AlarmEvent), s_detailStorage,
                                &s_detailControl);
  s_status = xQueueCreateStatic(1, sizeof(SafetyStatus), s_statusStorage, &s_statusControl);
  s_commands = xQueueCreateStatic(SAFETY_MAX_COMMANDS, sizeof(SafetyRequest), s_commandStorage,
                                  &s_commandControl);
  if (s_detail == NULL || s_status == NULL || s_commands == NULL) return false;
  journalInit(&s_journal);

  // Lamp policy from the applied configuration (compiled defaults in 8.2).
  configurationPeekApplied(&s_cfg);
  SafetyConfig config;
  memset(&config, 0, sizeof(config));
  config.lampAutoEnabled = s_cfg.lampAutoEnabled;
  config.lamp = s_cfg.lamp;
  config.lightStaleMs = POLICY_LIGHT_STALE_MS;
  safetyCoreInit(&s_core, &config, kPolicyRules, POLICY_RULE_COUNT, monoNowMs());
  relayWrite(false);

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_SAFETY);
  if (spec->stackBytes != sizeof(s_stack)) {
    logPrintf(LOG_LEVEL_WARN, "safety: stack %u B, task table says %lu B",
              (unsigned)sizeof(s_stack), (unsigned long)spec->stackBytes);
  }
  const BaseType_t safetyCore = spec->core;
  s_task = xTaskCreateStaticPinnedToCore(safetyTask, spec->name, sizeof(s_stack), NULL,
                                         spec->priority, s_stack, &s_tcb, safetyCore);
  if (s_task == NULL) return false;
  logPrintf(LOG_LEVEL_INFO, "safety: started (core %u, prio %u), lamp auto %s, relay OFF",
            (unsigned)safetyCore, (unsigned)spec->priority, s_cfg.lampAutoEnabled ? "ON" : "off");
  return true;
}

bool safetyStatusPeek(SafetyStatus* out) {
  if (s_status == NULL || out == NULL) return false;
  return xQueuePeek(s_status, out, 0) == pdTRUE;
}

#include "transports/ble_transport.h"

#ifndef FW_ENABLE_BLE

bool bleTransportInit(void) { return false; }
void bleTransportGetStats(BleTransportStats* out) { *out = (BleTransportStats){0}; }
void* bleTransportTaskHandle(void) { return 0; }

#else

#include <Arduino.h>
#include <ctype.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "board_profile.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "domain/cal_capture.h"
#include "domain/frame_codec.h"
#include "domain/protocol_codec.h"
#include "domain/sensor_set.h"
#include "hal/clock.h"
#include "policy_config.h"
#include "services/acquisition.h"
#include "services/configuration.h"
#include "services/rail_state.h"
#include "services/safety.h"
#include "services/snapshot_store.h"
#include "services/supervisor.h"
#include "transports/ble_gatt.h"
#include "transports/frames.h"
#include "transports/protocol.h"

#define BLE_NODE_NAME TRANSPORT_NODE_NAME
#define BLE_IDLE_CLIENT_MS 20000u  // connected but not subscribed after this: dropped
#define BLE_WAKE_MS 50u
#define BLE_RX_BYTES 2048
#define BLE_NOTIFY_RETRIES 40     // x 5 ms: NimBLE buffers free up as packets leave
#define BLE_COMMAND_TTL_MS 5000u  // not handled within this: refused, never applied late
#define BLE_PENDING 8             // = CFG_COMMAND_QUEUE_LEN: one slot per queued command
#define BLE_PENDING_LIMIT_MS (BLE_COMMAND_TTL_MS + 5000u)  // a reply that never came frees its slot
#define BLE_STATS_LOG_MS 60000u

// ---- RX: filled by the NimBLE host callback, drained by this task ----------------
static StaticStreamBuffer_t s_rxStruct;
static uint8_t s_rxStorage[BLE_RX_BYTES + 1];
static StreamBufferHandle_t s_rx;

// ---- Task and buffers (static: never on the 6 KB stack) ---------------------------
static StaticTask_t s_tcb;
static StackType_t s_stack[6 * 1024];  // cfgTaskSpec(TASK_ID_BLE_TRANSPORT)->stackBytes
static TaskHandle_t s_task = NULL;

static FrameAssembler s_assembler;
static uint8_t s_command[FRAME_MAX_PAYLOAD];
static uint8_t s_frame[FRAME_HEADER_BYTES + FRAME_MAX_PAYLOAD];
static char s_data[1536];
static TransportScratch s_scratch;
static ConfigBlob s_cfg;
static ConfigRequest s_req;  // static: a Wi-Fi password never sits on the stack
// static: get_diagnostics reports this task's own stack margin
static SupervisorDiagnostics s_diag;
static DiagnosticsView s_diagView;

static portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
static BleTransportStats s_stats;
#define STATS_ADD(field, n)          \
  do {                               \
    portENTER_CRITICAL(&s_statsMux); \
    s_stats.field += (n);            \
    portEXIT_CRITICAL(&s_statsMux);  \
  } while (0)

// ---- Commands in flight ----------------------------------------------------------
// Configuration and Safety answer from their own tasks; their callbacks only
// copy the outcome into s_results. The tag is slot | sequence << 8, so a late
// reply can never be matched to a slot that was freed and reused.
typedef struct Pending {
  bool used;
  CommandOp op;
  uint32_t generation;
  uint32_t seq;
  MonoMs sinceMs;
  char id[PROTOCOL_ID_MAX + 1];
} Pending;

typedef struct BleResult {
  uint32_t tag;
  ProtocolError error;
  uint16_t cfgRev;
} BleResult;

static Pending s_pending[BLE_PENDING];
static uint32_t s_seq;
static StaticQueue_t s_resultControl;
static uint8_t s_resultStorage[BLE_PENDING * sizeof(BleResult)];
static QueueHandle_t s_results;
static JournalCursor s_journalCursor;
static bool s_eventPending;    // an event failed to send and waits at the cursor
static bool s_eventLost;       // ... and its link closed: the next event reports gap:true
static uint32_t s_tornGeneration = UINT32_MAX;  // link whose stream was cut mid-frame
#ifdef FW_INJECT_BLE_TEAR
static uint32_t s_tearCount;  // env:ble_tear: telemetry frames sent
#endif

// NimBLE host task: copy and return (ble_gatt.h threading rule).
static void onRx(const uint8_t* data, size_t len, uint32_t generation, void* ctx) {
  (void)generation;
  (void)ctx;
  if (xStreamBufferSend(s_rx, data, len, 0) != len) STATS_ADD(rxOverflow, 1);
  if (s_task != NULL) xTaskNotifyGive(s_task);
}

static void postResult(const BleResult* r) {
  xQueueSend(s_results, r, 0);  // never full: one entry per pending slot
  if (s_task != NULL) xTaskNotifyGive(s_task);
}

static void onConfigResult(const ConfigResult* r, void* ctx) {
  (void)ctx;
  BleResult out;
  memset(&out, 0, sizeof(out));
  out.tag = r->tag;
  out.error = r->error;
  out.cfgRev = r->cfgRev;
  postResult(&out);
}

static void onSafetyResult(uint32_t tag, SafetyCommandOutcome outcome, void* ctx) {
  (void)ctx;
  BleResult out;
  memset(&out, 0, sizeof(out));
  out.tag = tag;
  out.error = outcome == SAFETY_CMD_APPLIED         ? PROTOCOL_OK
              : outcome == SAFETY_CMD_EXPIRED       ? PROTOCOL_ERR_EXPIRED
              : outcome == SAFETY_CMD_STILL_ACTIVE  ? PROTOCOL_ERR_RULE_ACTIVE
                                                    : PROTOCOL_ERR_BAD_FIELD;
  postResult(&out);
}

static uint32_t sessionNow(void) { return bleGattGeneration(); }

// ---- Sending ---------------------------------------------------------------------------
// Header + payload already in s_frame. Sent in MTU-sized chunks. A frame not
// started yet may be dropped (telemetry does not wait: the next frame
// replaces it). Once its first chunk is out, the frame must finish: the
// client reads each characteristic as one length-prefixed stream, so a cut
// frame would turn the next one into its continuation. If NimBLE buffers stay
// full mid-frame, the link is dropped instead (the app clears its decoders on
// reconnect) and nothing more is sent on it.
static bool sendFrame(BleChannel channel, size_t payloadLen, uint32_t generation) {
  if (payloadLen == 0 || !frameWriteHeader(s_frame, payloadLen)) return false;
  if (generation == s_tornGeneration) return false;  // being dropped
  const MonoMs start = monoNowMs();
  const size_t total = FRAME_HEADER_BYTES + payloadLen;
  bool tear = false;  // env:ble_tear: buffers "stay full" after the first chunk
#ifdef FW_INJECT_BLE_TEAR
  if (channel == BLE_CHANNEL_TELEMETRY && ++s_tearCount % (FW_INJECT_BLE_TEAR) == 0) tear = true;
#endif
  for (size_t offset = 0; offset < total;) {
    if (!bleGattConnected() || bleGattGeneration() != generation) return false;  // link gone: nothing to resync
    size_t chunk = bleGattChunkSize();
    if (chunk > total - offset) chunk = total - offset;
    int tries = 0;
    while ((tear && offset > 0) || !bleGattNotify(channel, &s_frame[offset], chunk)) {
      if (offset == 0 && channel == BLE_CHANNEL_TELEMETRY) return false;
      if (!bleGattConnected() || bleGattGeneration() != generation) return false;
      if (++tries >= BLE_NOTIFY_RETRIES) {
        if (offset > 0) {
          s_tornGeneration = generation;
          bleGattDisconnect();
          logPrintf(LOG_LEVEL_WARN, "ble: frame cut after %u of %u bytes - link dropped to resync",
                    (unsigned)offset, (unsigned)total);
        }
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    offset += chunk;
  }
  const uint32_t took = (uint32_t)(monoNowMs() - start);
  portENTER_CRITICAL(&s_statsMux);
  ++s_stats.framesSent;
  if (took > s_stats.maxSendMs) s_stats.maxSendMs = took;
  portEXIT_CRITICAL(&s_statsMux);
  return true;
}

static char* payload(void) { return (char*)&s_frame[FRAME_HEADER_BYTES]; }

static void sendResultFrame(size_t len, uint32_t generation) {
  if (!sendFrame(BLE_CHANNEL_RESULT, len, generation)) STATS_ADD(resultsDropped, 1);
}

static void sendError(const char* id, ProtocolError error, uint32_t generation) {
  sendResultFrame(protocolEncodeResultError(id, protocolErrorName(error), payload(), FRAME_MAX_PAYLOAD), generation);
}

static void sendBusy(const char* id, uint32_t generation) {
  STATS_ADD(busy, 1);
  sendResultFrame(protocolEncodeResultBusy(id, payload(), FRAME_MAX_PAYLOAD), generation);
}

// ---- State and telemetry -----------------------------------------------------------
static void publishState(bool notify, uint32_t generation) {
  const size_t len = transportEncodeState(&s_scratch, "ble", payload(), FRAME_MAX_PAYLOAD);
  if (len == 0 || !frameWriteHeader(s_frame, len)) return;
  bleGattSetStateValue(s_frame, FRAME_HEADER_BYTES + len);  // value for a plain read
  if (notify) sendFrame(BLE_CHANNEL_STATE, len, generation);
}

static void publishTelemetry(uint32_t generation) {
  const size_t len = transportEncodeTelemetry(&s_scratch, payload(), FRAME_MAX_PAYLOAD);
  if (len == 0) return;
  if (!sendFrame(BLE_CHANNEL_TELEMETRY, len, generation)) STATS_ADD(telemetryDropped, 1);
}

// ---- Commands -------------------------------------------------------------------------
static int claimSlot(const Command* cmd, uint32_t generation) {
  for (int i = 0; i < BLE_PENDING; ++i) {
    if (s_pending[i].used) continue;
    Pending* p = &s_pending[i];
    p->used = true;
    p->op = cmd->op;
    p->generation = generation;
    p->seq = ++s_seq & 0x00FFFFFFu;
    p->sinceMs = monoNowMs();
    memcpy(p->id, cmd->id, sizeof(p->id));
    return i;
  }
  return -1;
}

static uint32_t tagOf(int slot) { return (uint32_t)slot | (s_pending[slot].seq << 8); }

// Answered here, like get_sensors: a capture only peeks the snapshot (the raw
// level every calibratable driver publishes), it never touches a driver.
static void handleCapture(const Command* cmd, uint32_t generation) {
  configurationPeekApplied(&s_cfg);
  if (!sensorMaskHas(s_cfg.sensorEnabled, cmd->sensor) || !snapshotPeek(&s_scratch.snap)) {
    sendError(cmd->id, PROTOCOL_ERR_STALE, generation);
    return;
  }
  const RailState rail = railStateRead();
  snapshotGateRail(&s_scratch.snap, rail.valid5v, rail.epoch);  // never capture from a failed rail
  const CaptureLimits limits = {POLICY_CAL_CAPTURE_MAX_AGE_MS, CFG_STALE_ORDINARY_MS, BOARD_PH_DIVIDER_TOP_OHM,
                                BOARD_PH_DIVIDER_BOTTOM_OHM};
  float x;
  uint32_t ageMs;
  const CaptureStatus st = calCaptureFromSnapshot(cmd->sensor, &s_scratch.snap, monoNowMs(), &limits, &x, &ageMs);
  if (st != CAPTURE_OK) {
    sendError(cmd->id, st == CAPTURE_UNSUPPORTED ? PROTOCOL_ERR_BAD_FIELD : PROTOCOL_ERR_STALE, generation);
    return;
  }
  const int32_t scaled = (int32_t)(x * CAL_X_SCALE + (x >= 0.0f ? 0.5f : -0.5f));
  if (protocolEncodeCapture(cmd->sensor, scaled, ageMs, s_data, sizeof(s_data)) > 0) {
    sendResultFrame(protocolEncodeResultOk(cmd->id, s_data, payload(), FRAME_MAX_PAYLOAD), generation);
  }
}

// get_diagnostics: a copy of the Supervisor snapshot, measured nothing here.
static void handleDiagnostics(const Command* cmd, uint32_t generation) {
  supervisorGetDiagnostics(&s_diag);
  DiagnosticsView* v = &s_diagView;
  memset(v, 0, sizeof(*v));
  v->uptimeMs = monoNowMs();
  v->resetReason = s_diag.resetReason;
  v->cpuMhz = s_diag.cpuMhz;
  v->cpuValid = s_diag.cpuValid;
  for (uint8_t c = 0; c < DIAG_CORES && c < CPU_LOAD_CORES; ++c) {
    v->cpuLoad[c] = s_diag.cpu[c].loadPct;
    v->cpuPeak[c] = s_diag.cpu[c].peakPct;
  }
  v->memoryValid = s_diag.memoryValid;
  v->heapTotal = s_diag.heapTotal;
  v->heapFree = s_diag.heapFree;
  v->heapMin = s_diag.heapMin;
  v->heapLargest = s_diag.heapLargest;
  v->psramTotal = s_diag.psramTotal;
  v->psramFree = s_diag.psramFree;
  v->appPartition = s_diag.appPartition;
  v->nvsUsed = s_diag.nvsUsed;
  v->nvsTotal = s_diag.nvsTotal;
  for (TaskId id = 0; id < TASK_ID_COUNT && v->taskCount < DIAG_TASKS_MAX; ++id) {
    if (s_diag.stackFreeMin[id] == 0) continue;  // not created in this build, or not measured yet
    const TaskSpec* spec = cfgTaskSpec(id);
    DiagnosticsTask* t = &v->tasks[v->taskCount++];
    t->name = spec->name;
    t->core = spec->core;
    t->stackBytes = spec->stackBytes;
    t->freeMin = s_diag.stackFreeMin[id];
  }
  if (protocolEncodeDiagnostics(v, s_data, sizeof(s_data)) > 0) {
    sendResultFrame(protocolEncodeResultOk(cmd->id, s_data, payload(), FRAME_MAX_PAYLOAD), generation);
  }
}

static void handleCommand(size_t len, uint32_t generation) {
  Command cmd;
  const ProtocolError error = protocolParseCommand(s_command, len, &cmd);
  STATS_ADD(commands, 1);
  if (error != PROTOCOL_OK) {
    STATS_ADD(commandErrors, 1);
    logPrintf(LOG_LEVEL_INFO, "ble: command ? -> %s", protocolErrorName(error));
    sendError(cmd.id, error, generation);
    return;
  }
  logPrintf(LOG_LEVEL_INFO, "ble: command %s", commandOpName(cmd.op));  // never the fields (set_wifi)
  if (cmd.op == CMD_OP_GET_SENSORS) {
    configurationPeekApplied(&s_cfg);
    if (protocolEncodeSensorList(s_cfg.sensorEnabled, s_data, sizeof(s_data)) > 0) {
      sendResultFrame(protocolEncodeResultOk(cmd.id, s_data, payload(), FRAME_MAX_PAYLOAD), generation);
    }
    return;
  }
  if (cmd.op == CMD_OP_GET_CALIBRATION) {
    configurationPeekApplied(&s_cfg);
    const CalibrationView v = {s_cfg.ph, s_cfg.tds, s_cfg.light, s_cfg.calOverridden};
    if (protocolEncodeCalibration(&v, s_data, sizeof(s_data)) > 0) {
      sendResultFrame(protocolEncodeResultOk(cmd.id, s_data, payload(), FRAME_MAX_PAYLOAD), generation);
    }
    return;
  }
  if (cmd.op == CMD_OP_GET_DIAGNOSTICS) {
    handleDiagnostics(&cmd, generation);
    return;
  }
  if (cmd.op == CMD_OP_CAL_CAPTURE) {
    handleCapture(&cmd, generation);
    return;
  }
  const int slot = claimSlot(&cmd, generation);
  if (slot < 0) {
    sendBusy(cmd.id, generation);
    return;
  }
  const MonoMs expires = monoNowMs() + BLE_COMMAND_TTL_MS;
  bool queued;
  if (cmd.op == CMD_OP_RESET_LATCH) {
    const SafetyCommand sc = {SAFETY_CMD_RESET_LATCH, cmd.rule, expires, tagOf(slot)};
    queued = safetySubmitCommand(&sc, generation, sessionNow, onSafetyResult, NULL);  // dropped if the link closes
  } else {
    memset(&s_req, 0, sizeof(s_req));
    s_req.tag = tagOf(slot);
    s_req.session = generation;
    s_req.sessionNow = sessionNow;
    s_req.expiresMs = expires;
    s_req.reply = onConfigResult;
    switch (cmd.op) {
      case CMD_OP_SET_SENSOR_ENABLED:
        s_req.kind = CONFIG_REQ_SET_SENSOR_ENABLED;
        s_req.sensor = cmd.sensor;
        s_req.enabled = cmd.enabled;
        break;
      case CMD_OP_SET_WIFI:
        s_req.kind = CONFIG_REQ_SET_WIFI;
        memcpy(s_req.ssid, cmd.ssid, sizeof(s_req.ssid));
        memcpy(s_req.password, cmd.password, sizeof(s_req.password));
        break;
      case CMD_OP_FORGET_WIFI: s_req.kind = CONFIG_REQ_FORGET_WIFI; break;
      case CMD_OP_SET_CALIBRATION:
        s_req.kind = CONFIG_REQ_SET_CALIBRATION;
        s_req.sensor = cmd.sensor;
        s_req.calX1 = cmd.calX1;
        s_req.calY1 = cmd.calY1;
        s_req.calX2 = cmd.calX2;
        s_req.calY2 = cmd.calY2;
        break;
      default:
        s_req.kind = CONFIG_REQ_RESET_CALIBRATION;
        s_req.sensor = cmd.sensor;
        break;
    }
    queued = configurationSubmit(&s_req);
    memset(s_req.password, 0, sizeof(s_req.password));
  }
  memset(cmd.password, 0, sizeof(cmd.password));
  if (!queued) {
    s_pending[slot].used = false;
    sendBusy(cmd.id, generation);
  }
}

static void drainRx(uint32_t generation) {
  uint8_t chunk[128];
  size_t got;
  while ((got = xStreamBufferReceive(s_rx, chunk, sizeof(chunk), 0)) > 0) {
    size_t offset = 0;
    while (offset < got) {
      size_t used;
      const uint8_t* framePayload;
      size_t payloadLen;
      const FrameStatus status =
          frameAssemblerFeed(&s_assembler, &chunk[offset], got - offset, &used, &framePayload, &payloadLen);
      offset += used;
      if (status == FRAME_READY) {
        memcpy(s_command, framePayload, payloadLen);  // replies reuse s_frame
        handleCommand(payloadLen, generation);
      } else if (status == FRAME_ERROR) {
        STATS_ADD(commandErrors, 1);
        logPrintf(LOG_LEVEL_WARN, "ble: bad frame length from client, buffer dropped");
      }
    }
  }
}

static void drainResults(uint32_t generation) {
  BleResult r;
  while (xQueueReceive(s_results, &r, 0) == pdTRUE) {
    const uint32_t slot = r.tag & 0xFFu;
    if (slot >= BLE_PENDING) continue;
    Pending* p = &s_pending[slot];
    if (!p->used || p->seq != (r.tag >> 8)) continue;  // freed already: a late reply
    p->used = false;
    if (p->generation != generation) continue;  // asked on an earlier link: never answered on this one
    if (r.error != PROTOCOL_OK) {
      sendError(p->id, r.error, generation);
      logPrintf(LOG_LEVEL_INFO, "ble: %s -> %s", commandOpName(p->op), protocolErrorName(r.error));
      continue;
    }
    if (p->op == CMD_OP_SET_SENSOR_ENABLED || p->op == CMD_OP_SET_CALIBRATION ||
        p->op == CMD_OP_RESET_CALIBRATION) {
      snprintf(s_data, sizeof(s_data), "{\"applied\":true,\"cfg_rev\":%u}", (unsigned)r.cfgRev);
    } else if (p->op == CMD_OP_RESET_LATCH) {
      snprintf(s_data, sizeof(s_data), "{\"applied\":true}");
    } else {
      snprintf(s_data, sizeof(s_data), "{\"stored\":true}");
    }
    logPrintf(LOG_LEVEL_INFO, "ble: %s -> ok", commandOpName(p->op));
    sendResultFrame(protocolEncodeResultOk(p->id, s_data, payload(), FRAME_MAX_PAYLOAD), generation);
  }
}

// A link change orphans every request of the old link (Configuration drops
// stale-session requests without replying); a reply that never comes frees
// its slot after BLE_PENDING_LIMIT_MS.
static void expirePending(uint32_t generation, MonoMs now) {
  for (int i = 0; i < BLE_PENDING; ++i) {
    Pending* p = &s_pending[i];
    if (p->used && (p->generation != generation || now - p->sinceMs > BLE_PENDING_LIMIT_MS)) p->used = false;
  }
}

// Peek, send, then commit: an event that could not be sent stays at the
// cursor and is retried on the next wake (the journal reports a gap if it
// overruns meanwhile). If the link closes first, the next session starts at
// the journal head, so its first event reports gap:true.
static void sendEvents(uint32_t generation) {
  JournalEvent e;
  bool gap;
  JournalCursor next = s_journalCursor;
  while (safetyJournalNext(&next, &e, &gap)) {
    const size_t len = transportEncodeEvent(&e, gap || s_eventLost, payload(), FRAME_MAX_PAYLOAD);
    if (!sendFrame(BLE_CHANNEL_RESULT, len, generation)) {
      STATS_ADD(resultsDropped, 1);
      s_eventPending = true;
      break;
    }
    s_journalCursor = next;
    s_eventPending = false;
    s_eventLost = false;
  }
}

static void logStats(void) {
  BleGattStats g;
  BleTransportStats t;
  bleGattGetStats(&g);
  bleTransportGetStats(&t);
  logPrintf(LOG_LEVEL_INFO, "ble: ready=%d conn=%lu disc=%lu refused=%lu idle=%lu | frames %lu tlmDrop %lu "
            "resDrop %lu busy %lu maxSend %lums notifyFail %lu",
            bleGattReady(), (unsigned long)g.connects, (unsigned long)g.disconnects, (unsigned long)g.refused,
            (unsigned long)g.idleDropped, (unsigned long)t.framesSent,
            (unsigned long)t.telemetryDropped, (unsigned long)t.resultsDropped, (unsigned long)t.busy,
            (unsigned long)t.maxSendMs, (unsigned long)g.notifyFailures);
}

// ---- Task ------------------------------------------------------------------------------
static void bleTask(void* arg) {
  (void)arg;
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "ble: task watchdog add failed");
  logPrintf(LOG_LEVEL_INFO, "ble: advertising as %s (open link, no pairing)", BLE_NODE_NAME);

  uint32_t generation = bleGattGeneration();
  MonoMs nextState = 0, nextTelemetry = 0, nextStats = monoNowMs() + BLE_STATS_LOG_MS, connectedAt = 0;
  bool wasReady = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLE_WAKE_MS));
    const MonoMs now = monoNowMs();

    if (bleGattGeneration() != generation) {  // new session: drop half-received commands
      generation = bleGattGeneration();
      frameAssemblerReset(&s_assembler);
      xStreamBufferReset(s_rx);
      if (s_eventPending) s_eventLost = true;  // never delivered: the next link is told
      s_eventPending = false;
      connectedAt = now;
    }
    expirePending(generation, now);
    const bool ready = bleGattReady();
    if (ready != wasReady) {
      logPrintf(LOG_LEVEL_INFO, "ble: %s", ready ? "client connected and subscribed" : "link closed");
      wasReady = ready;
      if (ready) {
        nextState = now;  // State at once
        safetyJournalCursorInit(&s_journalCursor);  // events from now on, not history
      }
    }

    // Priority: commands and their results, alarm events, State, Telemetry.
    if (bleGattConnected()) drainRx(generation);
    drainResults(generation);
    if (ready) sendEvents(generation);
    if (now >= nextState) {
      publishState(ready, generation);
      nextState = now + CFG_BLE_STATE_PERIOD_MS;
    }
    if (ready && now >= nextTelemetry) {
      publishTelemetry(generation);
      nextTelemetry = now + CFG_BLE_TELEMETRY_PERIOD_MS;
    }

    // A client that never subscribes (an operating system keeping the link
    // with no app on it) would hold the only slot: drop it.
    if (bleGattConnected() && !ready && now - connectedAt >= BLE_IDLE_CLIENT_MS) {
      logPrintf(LOG_LEVEL_WARN, "ble: client connected %u s without subscribing - dropped",
                (unsigned)(BLE_IDLE_CLIENT_MS / 1000u));
      bleGattDropIdle();
      connectedAt = now;  // once per link: the disconnect bumps the generation
    }

    if (now >= nextStats) {
      logStats();
      nextStats = now + BLE_STATS_LOG_MS;
    }
    progressMark(TASK_ID_BLE_TRANSPORT);
    esp_task_wdt_reset();
  }
}

bool bleTransportInit(void) {
  if (s_task != NULL) return true;
  s_rx = xStreamBufferCreateStatic(BLE_RX_BYTES, 1, s_rxStorage, &s_rxStruct);
  s_results = xQueueCreateStatic(BLE_PENDING, sizeof(BleResult), s_resultStorage, &s_resultControl);
  if (s_rx == NULL || s_results == NULL) return false;
  frameAssemblerInit(&s_assembler);
  if (!bleGattStart(BLE_NODE_NAME, onRx, NULL)) {
    logPrintf(LOG_LEVEL_ERROR, "ble: NimBLE failed to start");
    return false;
  }
  publishState(false, bleGattGeneration());  // also fixes the shared boot id (radio running)  // readable State before the first client

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_BLE_TRANSPORT);
  if (spec->stackBytes != sizeof(s_stack)) {
    logPrintf(LOG_LEVEL_WARN, "ble: stack %u B, task table says %lu B", (unsigned)sizeof(s_stack),
              (unsigned long)spec->stackBytes);
  }
  s_task = xTaskCreateStaticPinnedToCore(bleTask, spec->name, sizeof(s_stack), NULL, spec->priority, s_stack,
                                         &s_tcb, spec->core);
  if (s_task == NULL) return false;
  TaskHandle_t host = xTaskGetHandle("nimble_host");
  logPrintf(LOG_LEVEL_INFO, "ble: started (core %u, prio %u), NimBLE host core %d prio %u",
            (unsigned)spec->core, (unsigned)spec->priority, host != NULL ? (int)xTaskGetAffinity(host) : -1,
            host != NULL ? (unsigned)uxTaskPriorityGet(host) : 0u);
  return true;
}

void bleTransportGetStats(BleTransportStats* out) {
  portENTER_CRITICAL(&s_statsMux);
  *out = s_stats;
  portEXIT_CRITICAL(&s_statsMux);
}

void* bleTransportTaskHandle(void) { return s_task; }

#endif  // FW_ENABLE_BLE

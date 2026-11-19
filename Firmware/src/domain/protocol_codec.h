// JSON encoders and command parser for contract v2 (FirmwareContract.md).
// Pure C, no heap, no cJSON (the native test env has no ESP SDK). WP8.
//
// Encoders write the JSON payload only (no length header) into a JsonWriter
// over a caller buffer. On overflow the writer stops and reports it; the
// caller must not send a truncated frame.
//
// The parser accepts one flat command object with an optional flat "data"
// object, validates every field the operation needs and ignores unknown keys.
// Anything malformed is rejected before it can reach Configuration or Safety.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/calibration.h"
#include "domain/reading.h"
#include "domain/sensor_set.h"

#define PROTOCOL_VERSION 2
#define PROTOCOL_ID_MAX 40
#define PROTOCOL_SSID_MAX 32        // bytes (802.11)
#define PROTOCOL_PASSWORD_MIN 8     // WPA2 passphrase, printable ASCII
#define PROTOCOL_PASSWORD_MAX 63
#define PROTOCOL_MAX_RULE_ID 15     // masks are 16 bits; Safety checks ruleCount

// ---------------------------------------------------------------- writer

typedef struct JsonWriter {
  char* buf;
  size_t cap;
  size_t len;
  bool overflow;
} JsonWriter;

void jsonWriterInit(JsonWriter* w, char* buf, size_t cap);
void jsonRaw(JsonWriter* w, const char* text);
void jsonString(JsonWriter* w, const char* s);  // quoted and escaped
void jsonKey(JsonWriter* w, const char* key);   // "key": (no comma handling)
void jsonU32(JsonWriter* w, uint32_t v);
void jsonI32(JsonWriter* w, int32_t v);
void jsonU64(JsonWriter* w, uint64_t v);
void jsonFloat(JsonWriter* w, float v);         // null for NaN / inf
void jsonBool(JsonWriter* w, bool v);
// Writes [i, j, ...] for the bits set in mask below `count`.
void jsonBitList(JsonWriter* w, uint16_t mask, uint8_t count);

// Payload length, or 0 if the writer overflowed.
static inline size_t jsonWriterLength(const JsonWriter* w) { return w->overflow ? 0 : w->len; }

// ---------------------------------------------------------------- encoders

typedef struct StateView {
  const char* node;
  const char* fw;
  uint32_t boot;
  MonoMs uptimeMs;
  const char* link;        // "ble" (the only data link)
  const char* mode;        // lowercase power mode name
  uint32_t powerEpoch;
  uint16_t cfgRev;
  const char* wifiState;   // unconfigured / connecting / connected / failed
  const char* wifiSsid;    // NULL or "" when unconfigured
  const char* wifiIp;      // only sent when connected
  int32_t wifiRssi;
  bool relayOn;
  const char* lampReason;
  bool interlock;
  uint16_t latchedMask;
  uint16_t activeMask;
  uint16_t staleMask;
  uint8_t ruleCount;
  const char* sgp30Baseline;
  SensorMask enabled;
} StateView;

size_t protocolEncodeState(const StateView* s, char* buf, size_t cap);

// All METRIC_COUNT entries keyed by metricName(). A value is sent only when
// the sensor is enabled, the stored health is OK or UNCALIBRATED and the
// sample is not older than alarmStaleLimitMs(); otherwise value is null and
// an aged OK/UNCALIBRATED reading is reported as STALE.
size_t protocolEncodeTelemetry(const Snapshot* snap, MonoMs now, uint32_t boot, uint16_t cfgRev,
                               SensorMask enabled, char* buf, size_t cap);

// status "ok" with dataJson (a complete JSON object, NULL means {}).
size_t protocolEncodeResultOk(const char* id, const char* dataJson, char* buf, size_t cap);
// status "error" with a short error code (see protocolErrorName()).
size_t protocolEncodeResultError(const char* id, const char* error, char* buf, size_t cap);
size_t protocolEncodeResultBusy(const char* id, char* buf, size_t cap);

typedef struct EventView {
  uint32_t n;
  uint8_t rule;
  const char* level;  // INFO / WARNING / CRITICAL
  const char* kind;   // trip / clear / reset
  uint32_t ageMs;
  bool gap;
} EventView;

size_t protocolEncodeEvent(const EventView* e, char* buf, size_t cap);

// {"sensors":[{"key","label","metrics":[...],"enabled","depends_note"},...]}
size_t protocolEncodeSensorList(SensorMask enabled, char* buf, size_t cap);

// Calibratable sensors (pH, TDS, light) and their stored fits.
typedef struct CalibrationView {
  LinearCalibration ph;
  LinearCalibration tds;
  LinearCalibration light;
  uint8_t overriddenMask;  // CAL_BIT_* : fitted on the device by the user
} CalibrationView;

// {"calibration":{"ph":{"overridden","rev","valid","slope","offset"},"tds":...,"light":...}}
size_t protocolEncodeCalibration(const CalibrationView* v, char* buf, size_t cap);

// {"sensor":"ph","x":<x scaled>,"age_ms":n}
size_t protocolEncodeCapture(SensorId sensor, int32_t xScaled, uint32_t ageMs, char* buf, size_t cap);

// Hardware diagnostics (get_diagnostics, Dashboard "Controller hardware").
// Filled by BleTransport from the Supervisor snapshot; null sections mean
// "not measured yet" (CPU about 2 s, memory about 10 s after boot).
#define DIAG_CORES 2
#define DIAG_TASKS_MAX 9

typedef struct DiagnosticsTask {
  const char* name;
  uint8_t core;
  uint32_t stackBytes;
  uint32_t freeMin;  // stack high-water mark, bytes
} DiagnosticsTask;

typedef struct DiagnosticsView {
  MonoMs uptimeMs;
  const char* resetReason;
  uint32_t cpuMhz;
  bool cpuValid;
  uint8_t cpuLoad[DIAG_CORES], cpuPeak[DIAG_CORES];
  bool memoryValid;
  uint32_t heapTotal, heapFree, heapMin, heapLargest;
  uint32_t psramTotal, psramFree;
  uint32_t appPartition, nvsUsed, nvsTotal;
  uint8_t taskCount;
  DiagnosticsTask tasks[DIAG_TASKS_MAX];
} DiagnosticsView;

// {"uptime_ms","reset_reason","cpu_mhz","cpu":[{"load","peak"},..]|null,
//  "heap":{"total","free","min","largest"}|null,"psram":{"total","free"},
//  "flash":{"app_partition","nvs_used","nvs_total"},
//  "stacks":[{"task","core","size","free_min"},..]}
size_t protocolEncodeDiagnostics(const DiagnosticsView* v, char* buf, size_t cap);

// ---------------------------------------------------------------- commands

typedef uint8_t CommandOp;
enum {
  CMD_OP_NONE = 0,
  CMD_OP_GET_SENSORS,
  CMD_OP_SET_SENSOR_ENABLED,
  CMD_OP_RESET_LATCH,
  CMD_OP_SET_WIFI,
  CMD_OP_FORGET_WIFI,
  CMD_OP_GET_CALIBRATION,
  CMD_OP_CAL_CAPTURE,
  CMD_OP_SET_CALIBRATION,
  CMD_OP_RESET_CALIBRATION,
  CMD_OP_GET_DIAGNOSTICS
};

// Calibration commands carry scaled integers (the parser keeps no floats):
//   ph     x = module mV x 10,           y = pH x 1000   (two points)
//   tds    x = uncalibrated ppm x 10,    y = ppm x 10    (one point, K)
//   light  x = pin mV x 10,              y = lux x 10    (one point, gain)
// cal_capture returns x in the same scale.
#define CAL_X_SCALE 10.0f
#define CAL_PH_Y_SCALE 1000.0f
#define CAL_Y_SCALE 10.0f

typedef uint8_t ProtocolError;
enum {
  PROTOCOL_OK = 0,
  PROTOCOL_ERR_BAD_REQUEST,   // not JSON / not an object / wrong version / no id
  PROTOCOL_ERR_UNKNOWN_OP,
  PROTOCOL_ERR_BAD_FIELD,     // missing, wrong type or out of range
  PROTOCOL_ERR_RULE_LATCHED,
  PROTOCOL_ERR_RULE_ACTIVE,
  PROTOCOL_ERR_EXPIRED,
  PROTOCOL_ERR_TIMEOUT,
  PROTOCOL_ERR_STORAGE,
  PROTOCOL_ERR_STALE,         // cal_capture: no fresh usable reading (or TDS without water temperature)
  PROTOCOL_ERR_BAD_FIT        // set_calibration: the points give an implausible fit
};

typedef struct Command {
  CommandOp op;
  char id[PROTOCOL_ID_MAX + 1];  // set as soon as it parses, even on later errors
  SensorId sensor;
  bool enabled;
  uint8_t rule;
  char ssid[PROTOCOL_SSID_MAX + 1];
  char password[PROTOCOL_PASSWORD_MAX + 1];
  int32_t calX1, calY1, calX2, calY2;  // set_calibration, scaled (above)
  bool calTwoPoints;
} Command;

// Sensors that take a calibration command: SENSOR_PH, SENSOR_TDS, SENSOR_LIGHT.
bool protocolSensorCalibratable(SensorId s);

ProtocolError protocolParseCommand(const uint8_t* payload, size_t len, Command* out);

// Contract error string ("bad_request", "rule_latched", ...).
const char* protocolErrorName(ProtocolError e);

const char* commandOpName(CommandOp op);

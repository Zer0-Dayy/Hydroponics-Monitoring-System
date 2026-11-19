#include "domain/protocol_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "domain/alarm.h"

// ================================================================ writer

void jsonWriterInit(JsonWriter* w, char* buf, size_t cap) {
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
  w->overflow = cap == 0;
  if (cap > 0) buf[0] = '\0';
}

static void put(JsonWriter* w, const char* text, size_t n) {
  if (w->overflow) return;
  if (w->len + n + 1 > w->cap) {  // keep room for the terminator
    w->overflow = true;
    return;
  }
  memcpy(&w->buf[w->len], text, n);
  w->len += n;
  w->buf[w->len] = '\0';
}

void jsonRaw(JsonWriter* w, const char* text) { put(w, text, strlen(text)); }

void jsonString(JsonWriter* w, const char* s) {
  put(w, "\"", 1);
  if (s != NULL) {
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; ++p) {
      switch (*p) {
        case '"': put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2); break;
        case '\r': put(w, "\\r", 2); break;
        case '\t': put(w, "\\t", 2); break;
        default:
          if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
            put(w, esc, 6);
          } else {
            put(w, (const char*)p, 1);  // UTF-8 bytes pass through
          }
      }
    }
  }
  put(w, "\"", 1);
}

void jsonKey(JsonWriter* w, const char* key) {
  jsonString(w, key);
  put(w, ":", 1);
}

void jsonU32(JsonWriter* w, uint32_t v) {
  char tmp[12];
  const int n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v);
  put(w, tmp, (size_t)n);
}

void jsonI32(JsonWriter* w, int32_t v) {
  char tmp[12];
  const int n = snprintf(tmp, sizeof(tmp), "%ld", (long)v);
  put(w, tmp, (size_t)n);
}

void jsonU64(JsonWriter* w, uint64_t v) {
  char tmp[24];
  const int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
  put(w, tmp, (size_t)n);
}

void jsonFloat(JsonWriter* w, float v) {
  if (isnan(v) || isinf(v)) {
    put(w, "null", 4);
    return;
  }
  char tmp[24];
  const int n = snprintf(tmp, sizeof(tmp), "%.7g", (double)v);  // float has ~7 digits
  put(w, tmp, (size_t)n);
}

void jsonBool(JsonWriter* w, bool v) {
  if (v) {
    put(w, "true", 4);
  } else {
    put(w, "false", 5);
  }
}

void jsonBitList(JsonWriter* w, uint16_t mask, uint8_t count) {
  put(w, "[", 1);
  bool first = true;
  for (uint8_t i = 0; i < count && i < 16; ++i) {
    if ((mask & (1u << i)) == 0) continue;
    if (!first) put(w, ",", 1);
    jsonU32(w, i);
    first = false;
  }
  put(w, "]", 1);
}

// ================================================================ encoders

static void header(JsonWriter* w, const char* type) {
  jsonRaw(w, "{\"v\":2,\"type\":");
  jsonString(w, type);
}

size_t protocolEncodeState(const StateView* s, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  header(&w, "state");
  jsonRaw(&w, ",");
  jsonKey(&w, "node");
  jsonString(&w, s->node);
  jsonRaw(&w, ",");
  jsonKey(&w, "fw");
  jsonString(&w, s->fw);
  jsonRaw(&w, ",");
  jsonKey(&w, "boot");
  jsonU32(&w, s->boot);
  jsonRaw(&w, ",");
  jsonKey(&w, "uptime_ms");
  jsonU64(&w, s->uptimeMs);
  jsonRaw(&w, ",");
  jsonKey(&w, "link");
  jsonString(&w, s->link);
  jsonRaw(&w, ",");
  jsonKey(&w, "mode");
  jsonString(&w, s->mode);
  jsonRaw(&w, ",");
  jsonKey(&w, "power_epoch");
  jsonU32(&w, s->powerEpoch);
  jsonRaw(&w, ",");
  jsonKey(&w, "cfg_rev");
  jsonU32(&w, s->cfgRev);

  jsonRaw(&w, ",\"wifi\":{");
  jsonKey(&w, "state");
  jsonString(&w, s->wifiState);
  if (s->wifiSsid != NULL && s->wifiSsid[0] != '\0') {
    jsonRaw(&w, ",");
    jsonKey(&w, "ssid");
    jsonString(&w, s->wifiSsid);
  }
  if (s->wifiIp != NULL && s->wifiState != NULL && strcmp(s->wifiState, "connected") == 0) {
    jsonRaw(&w, ",");
    jsonKey(&w, "ip");
    jsonString(&w, s->wifiIp);
    jsonRaw(&w, ",");
    jsonKey(&w, "rssi");
    jsonI32(&w, s->wifiRssi);
  }
  jsonRaw(&w, "}");

  jsonRaw(&w, ",\"safety\":{");
  jsonKey(&w, "relay");
  jsonBool(&w, s->relayOn);
  jsonRaw(&w, ",");
  jsonKey(&w, "lamp_reason");
  jsonString(&w, s->lampReason);
  jsonRaw(&w, ",");
  jsonKey(&w, "interlock");
  jsonBool(&w, s->interlock);
  jsonRaw(&w, ",");
  jsonKey(&w, "latched");
  jsonBitList(&w, s->latchedMask, s->ruleCount);
  jsonRaw(&w, ",");
  jsonKey(&w, "active");
  jsonBitList(&w, s->activeMask, s->ruleCount);
  jsonRaw(&w, ",");
  jsonKey(&w, "stale");
  jsonBitList(&w, s->staleMask, s->ruleCount);
  jsonRaw(&w, "}");

  jsonRaw(&w, ",");
  jsonKey(&w, "sgp30_baseline");
  jsonString(&w, s->sgp30Baseline);
  jsonRaw(&w, ",\"enabled\":[");
  bool first = true;
  for (uint8_t i = 0; i < SENSOR_COUNT; ++i) {
    if (!sensorMaskHas(s->enabled, (SensorId)i)) continue;
    if (!first) jsonRaw(&w, ",");
    jsonString(&w, sensorKey((SensorId)i));
    first = false;
  }
  jsonRaw(&w, "]}");
  return jsonWriterLength(&w);
}

static void encodeMetric(JsonWriter* w, const Reading* r, MonoMs now, bool enabled) {
  const bool measured = r->intervalEndMs != 0 && r->health != HEALTH_STARTING;
  const MonoMs age = measured && now > r->intervalEndMs ? now - r->intervalEndMs : 0;
  Health health = r->health;
  bool usable = health == HEALTH_OK || health == HEALTH_UNCALIBRATED;
  if (usable && age > alarmStaleLimitMs(r->metric)) {
    health = HEALTH_STALE;  // never send a retained value as a new observation (§10)
    usable = false;
  }

  jsonKey(w, metricName(r->metric));
  jsonRaw(w, "{\"value\":");
  if (usable && enabled) {
    jsonFloat(w, r->value);
  } else {
    jsonRaw(w, "null");
  }
  jsonRaw(w, ",");
  jsonKey(w, "unit");
  jsonString(w, unitName(r->unit));
  jsonRaw(w, ",");
  jsonKey(w, "health");
  jsonString(w, healthName(health));
  jsonRaw(w, ",");
  jsonKey(w, "age_ms");
  if (measured) {
    jsonU64(w, age);
  } else {
    jsonRaw(w, "null");
  }
  jsonRaw(w, ",");
  jsonKey(w, "seq");
  jsonU32(w, r->sequence);
  jsonRaw(w, ",");
  jsonKey(w, "q");
  jsonU32(w, r->qualityFlags);
  jsonRaw(w, ",");
  jsonKey(w, "cal_rev");
  jsonU32(w, r->calibrationRevision);
  jsonRaw(w, "}");
}

size_t protocolEncodeTelemetry(const Snapshot* snap, MonoMs now, uint32_t boot, uint16_t cfgRev,
                               SensorMask enabled, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  header(&w, "telemetry");
  jsonRaw(&w, ",");
  jsonKey(&w, "boot");
  jsonU32(&w, boot);
  jsonRaw(&w, ",");
  jsonKey(&w, "cfg_rev");
  jsonU32(&w, cfgRev);
  jsonRaw(&w, ",\"m\":{");
  for (uint8_t i = 0; i < METRIC_COUNT; ++i) {
    if (i > 0) jsonRaw(&w, ",");
    const Reading* r = &snap->readings[i];
    encodeMetric(&w, r, now, sensorMaskHas(enabled, sensorOfMetric(r->metric)));
  }
  jsonRaw(&w, "}}");
  return jsonWriterLength(&w);
}

static void resultHead(JsonWriter* w, const char* id, const char* status) {
  header(w, "result");
  jsonRaw(w, ",");
  jsonKey(w, "id");
  jsonString(w, id);
  jsonRaw(w, ",");
  jsonKey(w, "status");
  jsonString(w, status);
}

size_t protocolEncodeResultOk(const char* id, const char* dataJson, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  resultHead(&w, id, "ok");
  jsonRaw(&w, ",\"data\":");
  jsonRaw(&w, dataJson != NULL ? dataJson : "{}");
  jsonRaw(&w, "}");
  return jsonWriterLength(&w);
}

size_t protocolEncodeResultError(const char* id, const char* error, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  resultHead(&w, id, "error");
  jsonRaw(&w, ",");
  jsonKey(&w, "error");
  jsonString(&w, error);
  jsonRaw(&w, "}");
  return jsonWriterLength(&w);
}

size_t protocolEncodeResultBusy(const char* id, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  resultHead(&w, id, "busy");
  jsonRaw(&w, "}");
  return jsonWriterLength(&w);
}

size_t protocolEncodeEvent(const EventView* e, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  header(&w, "event");
  jsonRaw(&w, ",");
  jsonKey(&w, "n");
  jsonU32(&w, e->n);
  jsonRaw(&w, ",");
  jsonKey(&w, "rule");
  jsonU32(&w, e->rule);
  jsonRaw(&w, ",");
  jsonKey(&w, "level");
  jsonString(&w, e->level);
  jsonRaw(&w, ",");
  jsonKey(&w, "kind");
  jsonString(&w, e->kind);
  jsonRaw(&w, ",");
  jsonKey(&w, "age_ms");
  jsonU32(&w, e->ageMs);
  jsonRaw(&w, ",");
  jsonKey(&w, "gap");
  jsonBool(&w, e->gap);
  jsonRaw(&w, "}");
  return jsonWriterLength(&w);
}

size_t protocolEncodeSensorList(SensorMask enabled, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  jsonRaw(&w, "{\"sensors\":[");
  for (uint8_t s = 0; s < SENSOR_COUNT; ++s) {
    if (s > 0) jsonRaw(&w, ",");
    jsonRaw(&w, "{");
    jsonKey(&w, "key");
    jsonString(&w, sensorKey((SensorId)s));
    jsonRaw(&w, ",");
    jsonKey(&w, "label");
    jsonString(&w, sensorLabel((SensorId)s));
    jsonRaw(&w, ",\"metrics\":[");
    bool first = true;
    for (uint8_t m = 0; m < METRIC_COUNT; ++m) {
      if (sensorOfMetric((MetricId)m) != s) continue;
      if (!first) jsonRaw(&w, ",");
      jsonString(&w, metricName((MetricId)m));
      first = false;
    }
    jsonRaw(&w, "],");
    jsonKey(&w, "enabled");
    jsonBool(&w, sensorMaskHas(enabled, (SensorId)s));
    jsonRaw(&w, ",");
    jsonKey(&w, "depends_note");
    jsonString(&w, sensorDependsNote((SensorId)s));
    jsonRaw(&w, "}");
  }
  jsonRaw(&w, "]}");
  return jsonWriterLength(&w);
}

static void encodeFit(JsonWriter* w, const char* key, const LinearCalibration* c, bool overridden) {
  jsonKey(w, key);
  jsonRaw(w, "{");
  jsonKey(w, "overridden");
  jsonBool(w, overridden);
  jsonRaw(w, ",");
  jsonKey(w, "rev");
  jsonU32(w, c->revision);
  jsonRaw(w, ",");
  jsonKey(w, "valid");
  jsonBool(w, c->valid);
  jsonRaw(w, ",");
  jsonKey(w, "slope");
  jsonFloat(w, c->slope);
  jsonRaw(w, ",");
  jsonKey(w, "offset");
  jsonFloat(w, c->offset);
  jsonRaw(w, "}");
}

size_t protocolEncodeCalibration(const CalibrationView* v, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  jsonRaw(&w, "{\"calibration\":{");
  encodeFit(&w, "ph", &v->ph, (v->overriddenMask & CAL_BIT_PH) != 0);
  jsonRaw(&w, ",");
  encodeFit(&w, "tds", &v->tds, (v->overriddenMask & CAL_BIT_TDS) != 0);
  jsonRaw(&w, ",");
  encodeFit(&w, "light", &v->light, (v->overriddenMask & CAL_BIT_LIGHT) != 0);
  jsonRaw(&w, "}}");
  return jsonWriterLength(&w);
}

size_t protocolEncodeCapture(SensorId sensor, int32_t xScaled, uint32_t ageMs, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  jsonRaw(&w, "{");
  jsonKey(&w, "sensor");
  jsonString(&w, sensorKey(sensor));
  jsonRaw(&w, ",");
  jsonKey(&w, "x");
  jsonI32(&w, xScaled);
  jsonRaw(&w, ",");
  jsonKey(&w, "age_ms");
  jsonU32(&w, ageMs);
  jsonRaw(&w, "}");
  return jsonWriterLength(&w);
}

static void encodeKeyU32(JsonWriter* w, const char* key, uint32_t v, bool comma) {
  if (comma) jsonRaw(w, ",");
  jsonKey(w, key);
  jsonU32(w, v);
}

size_t protocolEncodeDiagnostics(const DiagnosticsView* v, char* buf, size_t cap) {
  JsonWriter w;
  jsonWriterInit(&w, buf, cap);
  jsonRaw(&w, "{");
  jsonKey(&w, "uptime_ms");
  jsonU64(&w, v->uptimeMs);
  jsonRaw(&w, ",");
  jsonKey(&w, "reset_reason");
  jsonString(&w, v->resetReason != NULL ? v->resetReason : "UNKNOWN");
  encodeKeyU32(&w, "cpu_mhz", v->cpuMhz, true);
  jsonRaw(&w, ",\"cpu\":");
  if (v->cpuValid) {
    jsonRaw(&w, "[");
    for (uint8_t c = 0; c < DIAG_CORES; ++c) {
      if (c > 0) jsonRaw(&w, ",");
      jsonRaw(&w, "{");
      encodeKeyU32(&w, "load", v->cpuLoad[c], false);
      encodeKeyU32(&w, "peak", v->cpuPeak[c], true);
      jsonRaw(&w, "}");
    }
    jsonRaw(&w, "]");
  } else {
    jsonRaw(&w, "null");
  }
  jsonRaw(&w, ",\"heap\":");
  if (v->memoryValid) {
    jsonRaw(&w, "{");
    encodeKeyU32(&w, "total", v->heapTotal, false);
    encodeKeyU32(&w, "free", v->heapFree, true);
    encodeKeyU32(&w, "min", v->heapMin, true);
    encodeKeyU32(&w, "largest", v->heapLargest, true);
    jsonRaw(&w, "}");
  } else {
    jsonRaw(&w, "null");
  }
  jsonRaw(&w, ",\"psram\":{");
  encodeKeyU32(&w, "total", v->psramTotal, false);
  encodeKeyU32(&w, "free", v->psramFree, true);
  jsonRaw(&w, "},\"flash\":{");
  encodeKeyU32(&w, "app_partition", v->appPartition, false);
  encodeKeyU32(&w, "nvs_used", v->nvsUsed, true);
  encodeKeyU32(&w, "nvs_total", v->nvsTotal, true);
  jsonRaw(&w, "},\"stacks\":[");
  const uint8_t n = v->taskCount < DIAG_TASKS_MAX ? v->taskCount : DIAG_TASKS_MAX;
  for (uint8_t i = 0; i < n; ++i) {
    const DiagnosticsTask* t = &v->tasks[i];
    if (i > 0) jsonRaw(&w, ",");
    jsonRaw(&w, "{");
    jsonKey(&w, "task");
    jsonString(&w, t->name != NULL ? t->name : "?");
    encodeKeyU32(&w, "core", t->core, true);
    encodeKeyU32(&w, "size", t->stackBytes, true);
    encodeKeyU32(&w, "free_min", t->freeMin, true);
    jsonRaw(&w, "}");
  }
  jsonRaw(&w, "]}");
  return jsonWriterLength(&w);
}

// ================================================================ parser

#define FIELD_KEY_MAX 15
#define FIELD_STR_MAX 64
#define DATA_FIELDS_MAX 8
#define SKIP_DEPTH_MAX 4

typedef uint8_t JsonType;
enum { JT_NONE = 0, JT_STRING, JT_NUMBER, JT_BOOL, JT_NULL, JT_OBJECT, JT_ARRAY };

typedef struct Field {
  char key[FIELD_KEY_MAX + 1];
  JsonType type;
  char str[FIELD_STR_MAX + 1];  // decoded UTF-8 for JT_STRING
  size_t strLen;
  bool strTooLong;
  bool isInteger;               // JT_NUMBER without fraction/exponent, in int32 range
  int32_t integer;
  bool boolean;
} Field;

typedef struct Parser {
  const uint8_t* p;
  const uint8_t* end;
} Parser;

static void skipWs(Parser* ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
    ++ps->p;
  }
}

static bool expect(Parser* ps, char c) {
  skipWs(ps);
  if (ps->p >= ps->end || *ps->p != (uint8_t)c) return false;
  ++ps->p;
  return true;
}

static bool hexValue(uint8_t c, uint32_t* out) {
  if (c >= '0' && c <= '9') *out = (uint32_t)(c - '0');
  else if (c >= 'a' && c <= 'f') *out = (uint32_t)(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') *out = (uint32_t)(c - 'A' + 10);
  else return false;
  return true;
}

static void appendByte(char* out, size_t cap, size_t* len, bool* tooLong, uint8_t b) {
  if (*len < cap) {
    out[*len] = (char)b;
  } else {
    *tooLong = true;
  }
  ++*len;
}

// Parses a JSON string at ps->p (opening quote included) into out (cap bytes,
// always NUL-terminated within cap+1). Surrogate pairs are not supported (no
// field the firmware reads needs characters outside the BMP).
static bool parseString(Parser* ps, char* out, size_t cap, size_t* lenOut, bool* tooLong) {
  size_t len = 0;
  *tooLong = false;
  if (!expect(ps, '"')) return false;
  while (ps->p < ps->end) {
    const uint8_t c = *ps->p++;
    if (c == '"') {
      out[len < cap ? len : cap] = '\0';
      *lenOut = len;
      return true;
    }
    if (c < 0x20) return false;
    if (c != '\\') {
      appendByte(out, cap, &len, tooLong, c);
      continue;
    }
    if (ps->p >= ps->end) return false;
    const uint8_t e = *ps->p++;
    switch (e) {
      case '"': appendByte(out, cap, &len, tooLong, '"'); break;
      case '\\': appendByte(out, cap, &len, tooLong, '\\'); break;
      case '/': appendByte(out, cap, &len, tooLong, '/'); break;
      case 'b': appendByte(out, cap, &len, tooLong, '\b'); break;
      case 'f': appendByte(out, cap, &len, tooLong, '\f'); break;
      case 'n': appendByte(out, cap, &len, tooLong, '\n'); break;
      case 'r': appendByte(out, cap, &len, tooLong, '\r'); break;
      case 't': appendByte(out, cap, &len, tooLong, '\t'); break;
      case 'u': {
        if (ps->end - ps->p < 4) return false;
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
          uint32_t nib;
          if (!hexValue(*ps->p++, &nib)) return false;
          cp = (cp << 4) | nib;
        }
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp < 0x80) {
          appendByte(out, cap, &len, tooLong, (uint8_t)cp);
        } else if (cp < 0x800) {
          appendByte(out, cap, &len, tooLong, (uint8_t)(0xC0 | (cp >> 6)));
          appendByte(out, cap, &len, tooLong, (uint8_t)(0x80 | (cp & 0x3F)));
        } else {
          appendByte(out, cap, &len, tooLong, (uint8_t)(0xE0 | (cp >> 12)));
          appendByte(out, cap, &len, tooLong, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
          appendByte(out, cap, &len, tooLong, (uint8_t)(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default: return false;
    }
  }
  return false;  // unterminated
}

static bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

static bool parseNumber(Parser* ps, Field* f) {
  const uint8_t* p = ps->p;
  bool negative = false;
  if (p < ps->end && *p == '-') {
    negative = true;
    ++p;
  }
  if (p >= ps->end || !isDigit(*p)) return false;
  int64_t value = 0;
  bool overflow = false;
  if (*p == '0') {
    ++p;
  } else {
    while (p < ps->end && isDigit(*p)) {
      // Past the supported range: stop accumulating (no signed overflow on a
      // long number), keep validating the digits.
      if (!overflow) {
        value = value * 10 + (*p - '0');
        if (value > 2147483648LL) overflow = true;
      }
      ++p;
    }
  }
  bool integer = true;
  if (p < ps->end && *p == '.') {
    integer = false;
    ++p;
    if (p >= ps->end || !isDigit(*p)) return false;
    while (p < ps->end && isDigit(*p)) ++p;
  }
  if (p < ps->end && (*p == 'e' || *p == 'E')) {
    integer = false;
    ++p;
    if (p < ps->end && (*p == '+' || *p == '-')) ++p;
    if (p >= ps->end || !isDigit(*p)) return false;
    while (p < ps->end && isDigit(*p)) ++p;
  }
  ps->p = p;
  f->type = JT_NUMBER;
  if (negative) value = -value;
  f->isInteger = integer && !overflow && value >= INT32_MIN && value <= INT32_MAX;
  f->integer = f->isInteger ? (int32_t)value : 0;
  return true;
}

static bool matchLiteral(Parser* ps, const char* lit) {
  const size_t n = strlen(lit);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) return false;
  ps->p += n;
  return true;
}

static bool skipValue(Parser* ps, int depth);

static bool skipContainer(Parser* ps, int depth, char close, bool object) {
  if (depth > SKIP_DEPTH_MAX) return false;
  ++ps->p;  // opening bracket
  skipWs(ps);
  if (ps->p < ps->end && *ps->p == (uint8_t)close) {
    ++ps->p;
    return true;
  }
  for (;;) {
    if (object) {
      char key[FIELD_KEY_MAX + 1];
      size_t keyLen;
      bool tooLong;
      skipWs(ps);
      if (!parseString(ps, key, FIELD_KEY_MAX, &keyLen, &tooLong)) return false;
      if (!expect(ps, ':')) return false;
    }
    if (!skipValue(ps, depth + 1)) return false;
    skipWs(ps);
    if (ps->p >= ps->end) return false;
    if (*ps->p == ',') {
      ++ps->p;
      continue;
    }
    if (*ps->p == (uint8_t)close) {
      ++ps->p;
      return true;
    }
    return false;
  }
}

// Parses any scalar into f, or skips a container (type JT_OBJECT/JT_ARRAY).
static bool parseScalarOrSkip(Parser* ps, Field* f, int depth) {
  skipWs(ps);
  if (ps->p >= ps->end) return false;
  const uint8_t c = *ps->p;
  if (c == '"') {
    f->type = JT_STRING;
    return parseString(ps, f->str, FIELD_STR_MAX, &f->strLen, &f->strTooLong);
  }
  if (c == '{') {
    f->type = JT_OBJECT;
    return skipContainer(ps, depth, '}', true);
  }
  if (c == '[') {
    f->type = JT_ARRAY;
    return skipContainer(ps, depth, ']', false);
  }
  if (c == 't' && matchLiteral(ps, "true")) {
    f->type = JT_BOOL;
    f->boolean = true;
    return true;
  }
  if (c == 'f' && matchLiteral(ps, "false")) {
    f->type = JT_BOOL;
    f->boolean = false;
    return true;
  }
  if (c == 'n' && matchLiteral(ps, "null")) {
    f->type = JT_NULL;
    return true;
  }
  return parseNumber(ps, f);
}

static bool skipValue(Parser* ps, int depth) {
  Field scratch;
  memset(&scratch, 0, sizeof(scratch));
  return parseScalarOrSkip(ps, &scratch, depth);
}

// Parses the flat "data" object; nested values are skipped (type kept).
static bool parseData(Parser* ps, Field* fields, uint8_t* count) {
  *count = 0;
  if (!expect(ps, '{')) return false;
  skipWs(ps);
  if (ps->p < ps->end && *ps->p == '}') {
    ++ps->p;
    return true;
  }
  for (;;) {
    Field f;
    memset(&f, 0, sizeof(f));
    size_t keyLen;
    bool keyTooLong;
    skipWs(ps);
    if (!parseString(ps, f.key, FIELD_KEY_MAX, &keyLen, &keyTooLong)) return false;
    if (!expect(ps, ':')) return false;
    if (!parseScalarOrSkip(ps, &f, 2)) return false;
    if (!keyTooLong && *count < DATA_FIELDS_MAX) fields[(*count)++] = f;
    skipWs(ps);
    if (ps->p >= ps->end) return false;
    if (*ps->p == ',') {
      ++ps->p;
      continue;
    }
    if (*ps->p == '}') {
      ++ps->p;
      return true;
    }
    return false;
  }
}

static const Field* findField(const Field* fields, uint8_t count, const char* key) {
  for (uint8_t i = 0; i < count; ++i) {
    if (strcmp(fields[i].key, key) == 0) return &fields[i];
  }
  return NULL;
}

typedef struct OpName {
  const char* name;
  CommandOp op;
} OpName;

static const OpName kOps[] = {
    {"get_sensors", CMD_OP_GET_SENSORS},
    {"set_sensor_enabled", CMD_OP_SET_SENSOR_ENABLED},
    {"reset_latch", CMD_OP_RESET_LATCH},
    {"set_wifi", CMD_OP_SET_WIFI},
    {"forget_wifi", CMD_OP_FORGET_WIFI},
    {"get_calibration", CMD_OP_GET_CALIBRATION},
    {"cal_capture", CMD_OP_CAL_CAPTURE},
    {"set_calibration", CMD_OP_SET_CALIBRATION},
    {"reset_calibration", CMD_OP_RESET_CALIBRATION},
    {"get_diagnostics", CMD_OP_GET_DIAGNOSTICS},
};

const char* commandOpName(CommandOp op) {
  for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
    if (kOps[i].op == op) return kOps[i].name;
  }
  return "?";
}

static bool printableAscii(const char* s, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] > 0x7E) return false;
  }
  return true;
}

bool protocolSensorCalibratable(SensorId s) {
  return s == SENSOR_PH || s == SENSOR_TDS || s == SENSOR_LIGHT;
}

static bool sensorField(const Field* data, uint8_t count, Command* out) {
  const Field* s = findField(data, count, "sensor");
  if (s == NULL || s->type != JT_STRING || s->strTooLong) return false;
  out->sensor = sensorFromKey(s->str, s->strLen);
  return out->sensor < SENSOR_COUNT;
}

static bool intField(const Field* data, uint8_t count, const char* key, int32_t min, int32_t max, int32_t* out) {
  const Field* f = findField(data, count, key);
  if (f == NULL || f->type != JT_NUMBER || !f->isInteger || f->integer < min || f->integer > max) return false;
  *out = f->integer;
  return true;
}

// Ranges in the scaled units of protocol_codec.h (plausibility only: the fit
// itself is checked by Configuration).
static ProtocolError validateCalibration(const Field* data, uint8_t count, Command* out) {
  if (!sensorField(data, count, out) || !protocolSensorCalibratable(out->sensor)) return PROTOCOL_ERR_BAD_FIELD;
  bool ok;
  switch (out->sensor) {
    case SENSOR_PH:  // module 0-5 V, pH 0-14, two buffers
      out->calTwoPoints = true;
      ok = intField(data, count, "x1", 0, 50000, &out->calX1) && intField(data, count, "y1", 0, 14000, &out->calY1) &&
           intField(data, count, "x2", 0, 50000, &out->calX2) && intField(data, count, "y2", 0, 14000, &out->calY2);
      break;
    case SENSOR_TDS:  // 0.1-2000 ppm both ways
      ok = intField(data, count, "x1", 1, 20000, &out->calX1) && intField(data, count, "y1", 1, 20000, &out->calY1);
      break;
    default:  // light: pin 0.1-3300 mV, 0.1-100000 lux
      ok = intField(data, count, "x1", 1, 33000, &out->calX1) && intField(data, count, "y1", 1, 1000000, &out->calY1);
      break;
  }
  return ok ? PROTOCOL_OK : PROTOCOL_ERR_BAD_FIELD;
}

static ProtocolError validateData(const Field* data, uint8_t count, bool haveData, Command* out) {
  switch (out->op) {
    case CMD_OP_GET_SENSORS:
    case CMD_OP_FORGET_WIFI:
    case CMD_OP_GET_CALIBRATION:
    case CMD_OP_GET_DIAGNOSTICS:
      return PROTOCOL_OK;  // data ignored
    default: break;
  }
  if (!haveData) return PROTOCOL_ERR_BAD_FIELD;

  switch (out->op) {
    case CMD_OP_CAL_CAPTURE:
    case CMD_OP_RESET_CALIBRATION:
      return sensorField(data, count, out) && protocolSensorCalibratable(out->sensor) ? PROTOCOL_OK
                                                                                     : PROTOCOL_ERR_BAD_FIELD;
    case CMD_OP_SET_CALIBRATION: return validateCalibration(data, count, out);
    case CMD_OP_SET_SENSOR_ENABLED: {
      const Field* s = findField(data, count, "sensor");
      const Field* e = findField(data, count, "enabled");
      if (s == NULL || s->type != JT_STRING || s->strTooLong) return PROTOCOL_ERR_BAD_FIELD;
      if (e == NULL || e->type != JT_BOOL) return PROTOCOL_ERR_BAD_FIELD;
      out->sensor = sensorFromKey(s->str, s->strLen);
      if (out->sensor >= SENSOR_COUNT) return PROTOCOL_ERR_BAD_FIELD;
      out->enabled = e->boolean;
      return PROTOCOL_OK;
    }
    case CMD_OP_RESET_LATCH: {
      const Field* r = findField(data, count, "rule");
      if (r == NULL || r->type != JT_NUMBER || !r->isInteger || r->integer < 0 ||
          r->integer > PROTOCOL_MAX_RULE_ID) {
        return PROTOCOL_ERR_BAD_FIELD;
      }
      out->rule = (uint8_t)r->integer;
      return PROTOCOL_OK;
    }
    case CMD_OP_SET_WIFI: {
      const Field* s = findField(data, count, "ssid");
      const Field* p = findField(data, count, "password");
      if (s == NULL || s->type != JT_STRING || s->strTooLong || s->strLen == 0 ||
          s->strLen > PROTOCOL_SSID_MAX) {
        return PROTOCOL_ERR_BAD_FIELD;
      }
      if (p == NULL || p->type != JT_STRING || p->strTooLong || p->strLen < PROTOCOL_PASSWORD_MIN ||
          p->strLen > PROTOCOL_PASSWORD_MAX || !printableAscii(p->str, p->strLen)) {
        return PROTOCOL_ERR_BAD_FIELD;
      }
      memcpy(out->ssid, s->str, s->strLen + 1);
      memcpy(out->password, p->str, p->strLen + 1);
      return PROTOCOL_OK;
    }
    default: return PROTOCOL_ERR_UNKNOWN_OP;
  }
}

ProtocolError protocolParseCommand(const uint8_t* payload, size_t len, Command* out) {
  memset(out, 0, sizeof(*out));
  out->sensor = SENSOR_COUNT;
  if (payload == NULL || len == 0) return PROTOCOL_ERR_BAD_REQUEST;

  Parser ps = {payload, payload + len};
  Field data[DATA_FIELDS_MAX];
  uint8_t dataCount = 0;
  bool haveData = false, haveVersion = false, haveId = false, idValid = false;
  int32_t version = -1;
  char op[24] = {0};
  size_t opLen = 0;
  bool opSeen = false, opTooLong = false;

  if (!expect(&ps, '{')) return PROTOCOL_ERR_BAD_REQUEST;
  skipWs(&ps);
  if (ps.p < ps.end && *ps.p == '}') {
    ++ps.p;
  } else {
    for (;;) {
      char key[FIELD_KEY_MAX + 1];
      size_t keyLen;
      bool keyTooLong;
      skipWs(&ps);
      if (!parseString(&ps, key, FIELD_KEY_MAX, &keyLen, &keyTooLong)) return PROTOCOL_ERR_BAD_REQUEST;
      if (!expect(&ps, ':')) return PROTOCOL_ERR_BAD_REQUEST;
      skipWs(&ps);

      if (!keyTooLong && strcmp(key, "data") == 0 && ps.p < ps.end && *ps.p == '{') {
        if (!parseData(&ps, data, &dataCount)) return PROTOCOL_ERR_BAD_REQUEST;
        haveData = true;
      } else if (!keyTooLong && strcmp(key, "op") == 0 && ps.p < ps.end && *ps.p == '"') {
        if (!parseString(&ps, op, sizeof(op) - 1, &opLen, &opTooLong)) return PROTOCOL_ERR_BAD_REQUEST;
        opSeen = true;
      } else {
        Field f;
        memset(&f, 0, sizeof(f));
        if (!parseScalarOrSkip(&ps, &f, 1)) return PROTOCOL_ERR_BAD_REQUEST;
        if (!keyTooLong && strcmp(key, "v") == 0) {
          haveVersion = f.type == JT_NUMBER && f.isInteger;
          version = f.integer;
        } else if (!keyTooLong && strcmp(key, "id") == 0) {
          haveId = true;
          idValid = f.type == JT_STRING && !f.strTooLong && f.strLen > 0 &&
                    f.strLen <= PROTOCOL_ID_MAX && printableAscii(f.str, f.strLen);
          if (idValid) memcpy(out->id, f.str, f.strLen + 1);
        }
      }
      skipWs(&ps);
      if (ps.p >= ps.end) return PROTOCOL_ERR_BAD_REQUEST;
      if (*ps.p == ',') {
        ++ps.p;
        continue;
      }
      if (*ps.p == '}') {
        ++ps.p;
        break;
      }
      return PROTOCOL_ERR_BAD_REQUEST;
    }
  }
  skipWs(&ps);
  if (ps.p != ps.end) return PROTOCOL_ERR_BAD_REQUEST;  // trailing bytes
  if (!haveVersion || version != PROTOCOL_VERSION || !haveId || !idValid) {
    return PROTOCOL_ERR_BAD_REQUEST;
  }
  if (!opSeen || opTooLong) return PROTOCOL_ERR_UNKNOWN_OP;
  for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
    if (strlen(kOps[i].name) == opLen && memcmp(kOps[i].name, op, opLen) == 0) {
      out->op = kOps[i].op;
      break;
    }
  }
  if (out->op == CMD_OP_NONE) return PROTOCOL_ERR_UNKNOWN_OP;
  return validateData(data, dataCount, haveData, out);
}

const char* protocolErrorName(ProtocolError e) {
  switch (e) {
    case PROTOCOL_OK: return "ok";
    case PROTOCOL_ERR_BAD_REQUEST: return "bad_request";
    case PROTOCOL_ERR_UNKNOWN_OP: return "unknown_op";
    case PROTOCOL_ERR_BAD_FIELD: return "bad_field";
    case PROTOCOL_ERR_RULE_LATCHED: return "rule_latched";
    case PROTOCOL_ERR_RULE_ACTIVE: return "rule_active";
    case PROTOCOL_ERR_EXPIRED: return "expired";
    case PROTOCOL_ERR_TIMEOUT: return "timeout";
    case PROTOCOL_ERR_STORAGE: return "storage";
    case PROTOCOL_ERR_STALE: return "stale";
    case PROTOCOL_ERR_BAD_FIT: return "bad_fit";
  }
  return "?";
}

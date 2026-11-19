// Host unit tests for the WP8 domain modules (pio test -e native):
// frame reassembly, contract v2 encoders and command parser, configuration
// blob (schema 2, calibration, v1 migration), calibration fits and capture,
// event journal, backoff, and the Safety command / disabled-sensor rules.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <stddef.h>

#include "domain/backoff.h"
#include "domain/cal_capture.h"
#include "domain/calibration.h"
#include "domain/config_blob.h"
#include "domain/event_journal.h"
#include "domain/frame_codec.h"
#include "domain/protocol_codec.h"
#include "domain/safety_core.h"
#include "domain/sensor_set.h"

void setUp(void) {}
void tearDown(void) {}

static FrameAssembler s_fa;
static char s_out[4096];

static ProtocolError parse(const char* json, Command* cmd) {
  return protocolParseCommand((const uint8_t*)json, strlen(json), cmd);
}

// ------------------------------------------------------------ frames

static void test_frame_split_across_packets(void) {
  const uint8_t frame[] = {5, 0, 'h', 'e', 'l', 'l', 'o'};
  frameAssemblerInit(&s_fa);
  size_t used;
  const uint8_t* payload;
  size_t n;
  for (size_t i = 0; i < sizeof(frame) - 1; ++i) {
    TEST_ASSERT_EQUAL(FRAME_NEED_MORE, frameAssemblerFeed(&s_fa, &frame[i], 1, &used, &payload, &n));
    TEST_ASSERT_EQUAL(1, used);
  }
  TEST_ASSERT_EQUAL(FRAME_READY,
                    frameAssemblerFeed(&s_fa, &frame[sizeof(frame) - 1], 1, &used, &payload, &n));
  TEST_ASSERT_EQUAL(5, n);
  TEST_ASSERT_EQUAL_MEMORY("hello", payload, 5);
}

static void test_frame_two_in_one_packet(void) {
  const uint8_t data[] = {2, 0, 'a', 'b', 3, 0, 'c', 'd', 'e'};
  frameAssemblerInit(&s_fa);
  size_t used;
  const uint8_t* payload;
  size_t n;
  TEST_ASSERT_EQUAL(FRAME_READY, frameAssemblerFeed(&s_fa, data, sizeof(data), &used, &payload, &n));
  TEST_ASSERT_EQUAL(4, used);
  TEST_ASSERT_EQUAL_MEMORY("ab", payload, 2);
  TEST_ASSERT_EQUAL(FRAME_READY,
                    frameAssemblerFeed(&s_fa, data + used, sizeof(data) - used, &used, &payload, &n));
  TEST_ASSERT_EQUAL(5, used);
  TEST_ASSERT_EQUAL_MEMORY("cde", payload, 3);
}

static void test_frame_rejects_zero_and_oversize(void) {
  size_t used;
  const uint8_t* payload;
  size_t n;
  const uint8_t zero[] = {0, 0};
  const uint8_t big[] = {0x01, 0x10};  // 4097
  frameAssemblerInit(&s_fa);
  TEST_ASSERT_EQUAL(FRAME_ERROR, frameAssemblerFeed(&s_fa, zero, 2, &used, &payload, &n));
  TEST_ASSERT_EQUAL(FRAME_ERROR, frameAssemblerFeed(&s_fa, big, 2, &used, &payload, &n));
  TEST_ASSERT_EQUAL_UINT32(2, s_fa.rejected);
  uint8_t hdr[2];
  TEST_ASSERT_TRUE(frameWriteHeader(hdr, 4096));
  TEST_ASSERT_EQUAL_HEX8(0x00, hdr[0]);
  TEST_ASSERT_EQUAL_HEX8(0x10, hdr[1]);
  TEST_ASSERT_FALSE(frameWriteHeader(hdr, 4097));
  TEST_ASSERT_FALSE(frameWriteHeader(hdr, 0));
}

// ------------------------------------------------------------ encoders

static Snapshot s_snap;

static void test_telemetry_value_rules(void) {
  snapshotReset(&s_snap);
  Reading* air = &s_snap.readings[METRIC_AIR_TEMPERATURE];
  air->health = HEALTH_OK;
  air->value = 23.5f;
  air->intervalEndMs = 9900;
  air->sequence = 7;
  Reading* ph = &s_snap.readings[METRIC_PH];
  ph->health = HEALTH_UNCALIBRATED;
  ph->unit = UNIT_MILLIVOLT;
  ph->value = 1532.0f;
  ph->intervalEndMs = 5000;  // 5 s old: beyond the 3 s ordinary limit
  Reading* tds = &s_snap.readings[METRIC_TDS];
  tds->health = HEALTH_UNPOWERED;
  tds->value = 999.0f;  // retained, must not be sent
  tds->intervalEndMs = 9800;
  Reading* light = &s_snap.readings[METRIC_LIGHT_LEVEL];
  light->health = HEALTH_OK;
  light->value = 80.0f;
  light->intervalEndMs = 9900;

  const SensorMask enabled = (SensorMask)(SENSOR_MASK_ALL & ~(1u << SENSOR_LIGHT));
  const size_t n = protocolEncodeTelemetry(&s_snap, 10000, 42, 3, enabled, s_out, sizeof(s_out));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"air_temperature\":{\"value\":23.5,\"unit\":\"degC\","
                                     "\"health\":\"OK\",\"age_ms\":100,\"seq\":7"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"ph\":{\"value\":null,\"unit\":\"mV\",\"health\":\"STALE\""));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"tds\":{\"value\":null,\"unit\":\"ppm\",\"health\":\"UNPOWERED\""));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"light_level\":{\"value\":null"));  // disabled
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"tvoc\":{\"value\":null,\"unit\":\"ppb\",\"health\":\"STARTING\","
                                     "\"age_ms\":null"));
  TEST_ASSERT_NULL(strstr(s_out, "999"));
  TEST_ASSERT_TRUE(n < 1600);  // well inside one 4096-byte frame
}

static void test_telemetry_overflow_returns_zero(void) {
  snapshotReset(&s_snap);
  char small[64];
  TEST_ASSERT_EQUAL(0, protocolEncodeTelemetry(&s_snap, 0, 1, 1, SENSOR_MASK_ALL, small, sizeof(small)));
}

static void test_state_escapes_and_hides_ip_unless_connected(void) {
  StateView v;
  memset(&v, 0, sizeof(v));
  v.node = "Hydro \"ESP1\"";
  v.fw = "0.8.0";
  v.link = "ble";
  v.mode = "unknown";
  v.wifiState = "connecting";
  v.wifiSsid = "Green\\house";
  v.wifiIp = "192.168.1.42";
  v.lampReason = "level";
  v.latchedMask = 0x0002;
  v.ruleCount = 2;
  v.sgp30Baseline = "learning";
  v.enabled = (SensorMask)((1u << SENSOR_PH) | (1u << SENSOR_TDS));
  TEST_ASSERT_TRUE(protocolEncodeState(&v, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"node\":\"Hydro \\\"ESP1\\\"\""));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"ssid\":\"Green\\\\house\""));
  TEST_ASSERT_NULL(strstr(s_out, "192.168"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"latched\":[1]"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"enabled\":[\"ph\",\"tds\"]"));
  v.wifiState = "connected";
  v.wifiRssi = -61;
  TEST_ASSERT_TRUE(protocolEncodeState(&v, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"ip\":\"192.168.1.42\",\"rssi\":-61"));
}

static void test_results_and_sensor_list(void) {
  TEST_ASSERT_TRUE(protocolEncodeResultError("a1", "rule_latched", s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":2,\"type\":\"result\",\"id\":\"a1\",\"status\":\"error\",\"error\":\"rule_latched\"}", s_out);
  TEST_ASSERT_TRUE(protocolEncodeResultBusy("a2", s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"v\":2,\"type\":\"result\",\"id\":\"a2\",\"status\":\"busy\"}", s_out);
  TEST_ASSERT_TRUE(protocolEncodeSensorList(SENSOR_MASK_ALL, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"key\":\"bme280\""));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"metrics\":[\"air_temperature\",\"air_humidity\",\"air_pressure\"]"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"metrics\":[\"tvoc\",\"co2eq\"]"));
}

// ------------------------------------------------------------ parser

typedef struct BadCase {
  const char* json;
  ProtocolError expected;
} BadCase;

static void test_parse_valid_commands(void) {
  Command c;
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"x1\",\"op\":\"set_sensor_enabled\","
                          "\"data\":{\"sensor\":\"tds\",\"enabled\":false}}",
                          &c));
  TEST_ASSERT_EQUAL(CMD_OP_SET_SENSOR_ENABLED, c.op);
  TEST_ASSERT_EQUAL(SENSOR_TDS, c.sensor);
  TEST_ASSERT_FALSE(c.enabled);
  TEST_ASSERT_EQUAL_STRING("x1", c.id);

  // Key order is free; unknown keys and nested values are ignored.
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse(" { \"data\" : {\"rule\": 1, \"extra\": [1,{\"a\":2}]}, \"op\":\"reset_latch\","
                          " \"x\":null, \"id\":\"r\", \"v\":2 } ",
                          &c));
  TEST_ASSERT_EQUAL(CMD_OP_RESET_LATCH, c.op);
  TEST_ASSERT_EQUAL(1, c.rule);

  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"w\",\"op\":\"set_wifi\","
                          "\"data\":{\"ssid\":\"Caf\\u00e9\",\"password\":\"pa\\\"ss word\"}}",
                          &c));
  TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", c.ssid);
  TEST_ASSERT_EQUAL_STRING("pa\"ss word", c.password);

  TEST_ASSERT_EQUAL(PROTOCOL_OK, parse("{\"v\":2,\"id\":\"g\",\"op\":\"get_sensors\"}", &c));
  TEST_ASSERT_EQUAL(PROTOCOL_OK, parse("{\"v\":2,\"id\":\"g\",\"op\":\"get_calibration\"}", &c));
  TEST_ASSERT_EQUAL(CMD_OP_GET_CALIBRATION, c.op);
  TEST_ASSERT_EQUAL(PROTOCOL_OK, parse("{\"v\":2,\"id\":\"d\",\"op\":\"get_diagnostics\",\"data\":{}}", &c));
  TEST_ASSERT_EQUAL(CMD_OP_GET_DIAGNOSTICS, c.op);
  TEST_ASSERT_EQUAL_STRING("get_diagnostics", commandOpName(CMD_OP_GET_DIAGNOSTICS));
}

static void test_parse_calibration_commands(void) {
  Command c;
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"c\",\"op\":\"set_calibration\","
                          "\"data\":{\"sensor\":\"ph\",\"x1\":25000,\"y1\":7000,\"x2\":30130,\"y2\":4010}}",
                          &c));
  TEST_ASSERT_EQUAL(CMD_OP_SET_CALIBRATION, c.op);
  TEST_ASSERT_EQUAL(SENSOR_PH, c.sensor);
  TEST_ASSERT_TRUE(c.calTwoPoints);
  TEST_ASSERT_EQUAL_INT32(25000, c.calX1);
  TEST_ASSERT_EQUAL_INT32(4010, c.calY2);
  TEST_ASSERT_EQUAL(PROTOCOL_OK, parse("{\"v\":2,\"id\":\"c\",\"op\":\"set_calibration\","
                                       "\"data\":{\"sensor\":\"tds\",\"x1\":6500,\"y1\":7070}}",
                                       &c));
  TEST_ASSERT_EQUAL(SENSOR_TDS, c.sensor);
  TEST_ASSERT_FALSE(c.calTwoPoints);
  TEST_ASSERT_EQUAL(PROTOCOL_OK, parse("{\"v\":2,\"id\":\"c\",\"op\":\"set_calibration\","
                                       "\"data\":{\"sensor\":\"light\",\"x1\":12000,\"y1\":30000}}",
                                       &c));
  TEST_ASSERT_EQUAL(SENSOR_LIGHT, c.sensor);
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"c\",\"op\":\"cal_capture\",\"data\":{\"sensor\":\"light\"}}", &c));
  TEST_ASSERT_EQUAL(CMD_OP_CAL_CAPTURE, c.op);
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"c\",\"op\":\"reset_calibration\",\"data\":{\"sensor\":\"tds\"}}", &c));
  TEST_ASSERT_EQUAL(CMD_OP_RESET_CALIBRATION, c.op);

  static const BadCase kBad[] = {
      // pH needs both buffers
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_calibration\",\"data\":{\"sensor\":\"ph\",\"x1\":25000,\"y1\":7000}}",
       PROTOCOL_ERR_BAD_FIELD},
      // pH above 14
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_calibration\",\"data\":{\"sensor\":\"ph\",\"x1\":1,\"y1\":15000,"
       "\"x2\":2,\"y2\":4000}}",
       PROTOCOL_ERR_BAD_FIELD},
      // fractions are not accepted: values are scaled integers
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_calibration\",\"data\":{\"sensor\":\"tds\",\"x1\":650.5,\"y1\":707}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_calibration\",\"data\":{\"sensor\":\"tds\",\"x1\":0,\"y1\":707}}",
       PROTOCOL_ERR_BAD_FIELD},
      // sensors without a calibration
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_calibration\",\"data\":{\"sensor\":\"bme280\",\"x1\":1,\"y1\":1}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"cal_capture\",\"data\":{\"sensor\":\"hcsr04\"}}", PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"reset_calibration\"}", PROTOCOL_ERR_BAD_FIELD},
      // the LAN ops are gone
      {"{\"v\":2,\"id\":\"a\",\"op\":\"get_lan_token\"}", PROTOCOL_ERR_UNKNOWN_OP},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"hello\",\"data\":{\"token\":\"0123456789abcdef0123456789abcdef\"}}",
       PROTOCOL_ERR_UNKNOWN_OP},
  };
  for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); ++i) {
    char msg[16];
    snprintf(msg, sizeof(msg), "case %u", (unsigned)i);
    TEST_ASSERT_EQUAL_MESSAGE(kBad[i].expected, parse(kBad[i].json, &c), msg);
  }
  TEST_ASSERT_EQUAL_STRING("stale", protocolErrorName(PROTOCOL_ERR_STALE));
  TEST_ASSERT_EQUAL_STRING("bad_fit", protocolErrorName(PROTOCOL_ERR_BAD_FIT));
}

static void test_encode_calibration_and_capture(void) {
  const CalibrationView v = {{-0.0057f, 22.7f, 0, true}, {1.05f, 0.0f, 9, true}, {1.0f, 0.0f, 0, false}, CAL_BIT_TDS};
  TEST_ASSERT_TRUE(protocolEncodeCalibration(&v, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"ph\":{\"overridden\":false,\"rev\":0,\"valid\":true"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"tds\":{\"overridden\":true,\"rev\":9,\"valid\":true,\"slope\":1.05"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"light\":{\"overridden\":false,\"rev\":0,\"valid\":false"));
  TEST_ASSERT_TRUE(protocolEncodeCapture(SENSOR_PH, 25012, 412, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"sensor\":\"ph\",\"x\":25012,\"age_ms\":412}", s_out);
}

static void test_encode_diagnostics(void) {
  static DiagnosticsView v;
  memset(&v, 0, sizeof(v));
  v.uptimeMs = 734120;
  v.resetReason = "POWERON";
  v.cpuMhz = 240;
  TEST_ASSERT_TRUE(protocolEncodeDiagnostics(&v, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"cpu\":null,\"heap\":null,"));  // not measured yet
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"stacks\":[]}"));

  v.cpuValid = true;
  v.cpuLoad[0] = 7;
  v.cpuPeak[0] = 19;
  v.cpuLoad[1] = 12;
  v.cpuPeak[1] = 31;
  v.memoryValid = true;
  v.heapTotal = 327680;
  v.heapFree = 182340;
  v.heapMin = 160112;
  v.heapLargest = 110592;
  v.taskCount = 1;
  v.tasks[0] = (DiagnosticsTask){"Safety", 1, 3072, 812};
  TEST_ASSERT_TRUE(protocolEncodeDiagnostics(&v, s_out, sizeof(s_out)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(s_out, "{\"uptime_ms\":734120,\"reset_reason\":\"POWERON\",\"cpu_mhz\":240,"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"cpu\":[{\"load\":7,\"peak\":19},{\"load\":12,\"peak\":31}]"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"heap\":{\"total\":327680,\"free\":182340,\"min\":160112,\"largest\":110592}"));
  TEST_ASSERT_NOT_NULL(strstr(s_out, "\"stacks\":[{\"task\":\"Safety\",\"core\":1,\"size\":3072,\"free_min\":812}]}"));

  // Worst case (every number at its maximum, all tasks) fits BleTransport's 1536 B s_data.
  static const char* kNames[DIAG_TASKS_MAX] = {"Safety", "PowerCoord", "AcqNormal", "AcqLowPower", "WifiMqtt",
                                               "BleTransport", "Configuration", "Supervisor", "SerialLogger"};
  v.uptimeMs = UINT64_MAX;
  v.resetReason = "BROWNOUT";
  v.cpuMhz = v.heapTotal = v.heapFree = v.heapMin = v.heapLargest = UINT32_MAX;
  v.psramTotal = v.psramFree = v.appPartition = v.nvsUsed = v.nvsTotal = UINT32_MAX;
  v.cpuLoad[0] = v.cpuLoad[1] = v.cpuPeak[0] = v.cpuPeak[1] = 100;
  v.taskCount = DIAG_TASKS_MAX;
  for (int i = 0; i < DIAG_TASKS_MAX; ++i) v.tasks[i] = (DiagnosticsTask){kNames[i], 1, UINT32_MAX, UINT32_MAX};
  char small[1536];
  const size_t len = protocolEncodeDiagnostics(&v, small, sizeof(small));
  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(len < sizeof(small));
  TEST_ASSERT_EQUAL(0, protocolEncodeDiagnostics(&v, small, 64));  // overflow reports 0, never a cut frame
}

static void test_parse_rejects_malformed_and_out_of_range(void) {
  static const BadCase kCases[] = {
      {"", PROTOCOL_ERR_BAD_REQUEST},
      {"[]", PROTOCOL_ERR_BAD_REQUEST},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"get_sensors\"", PROTOCOL_ERR_BAD_REQUEST},  // truncated
      {"{\"v\":2,\"id\":\"a\",\"op\":\"get_sensors\"}x", PROTOCOL_ERR_BAD_REQUEST},  // trailing
      {"{\"v\":1,\"id\":\"a\",\"op\":\"get_sensors\"}", PROTOCOL_ERR_BAD_REQUEST},  // old version
      {"{\"v\":2,\"op\":\"get_sensors\"}", PROTOCOL_ERR_BAD_REQUEST},              // no id
      {"{\"v\":2,\"id\":\"a\"}", PROTOCOL_ERR_UNKNOWN_OP},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"add_device\"}", PROTOCOL_ERR_UNKNOWN_OP},  // v1 op
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_sensor_enabled\",\"data\":{\"sensor\":\"mq7\",\"enabled\":true}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_sensor_enabled\",\"data\":{\"sensor\":\"ph\",\"enabled\":1}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_sensor_enabled\"}", PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"reset_latch\",\"data\":{\"rule\":1.5}}", PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"reset_latch\",\"data\":{\"rule\":-1}}", PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"reset_latch\",\"data\":{\"rule\":16}}", PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_wifi\",\"data\":{\"ssid\":\"x\",\"password\":\"short\"}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_wifi\",\"data\":{\"ssid\":\"\",\"password\":\"longenough\"}}",
       PROTOCOL_ERR_BAD_FIELD},
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_wifi\",\"data\":{\"ssid\":\"123456789012345678901234567890123\","
       "\"password\":\"longenough\"}}",
       PROTOCOL_ERR_BAD_FIELD},  // 33-byte SSID
      {"{\"v\":2,\"id\":\"a\",\"op\":\"set_wifi\",\"data\":{\"ssid\":\"x\",\"password\":\"tab\\there!\"}}",
       PROTOCOL_ERR_BAD_FIELD},  // control character
      {"{\"v\":2,\"id\":\"a\\u0000\",\"op\":\"get_sensors\"}", PROTOCOL_ERR_BAD_REQUEST},
      {"{\"v\":2,\"id\":\"12345678901234567890123456789012345678901\",\"op\":\"get_sensors\"}",
       PROTOCOL_ERR_BAD_REQUEST},  // 41-char id
      {"{\"v\":2,\"id\":\"a\",\"op\":\"get_sensors\",\"d\":[[[[[[1]]]]]]}", PROTOCOL_ERR_BAD_REQUEST},
  };
  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    Command c;
    char msg[16];
    snprintf(msg, sizeof(msg), "case %u", (unsigned)i);
    TEST_ASSERT_EQUAL_MESSAGE(kCases[i].expected, parse(kCases[i].json, &c), msg);
  }
}

static void test_parse_keeps_id_for_error_reply(void) {
  Command c;
  TEST_ASSERT_EQUAL(PROTOCOL_ERR_BAD_FIELD,
                    parse("{\"v\":2,\"id\":\"keep-me\",\"op\":\"reset_latch\",\"data\":{}}", &c));
  TEST_ASSERT_EQUAL_STRING("keep-me", c.id);
  TEST_ASSERT_EQUAL_STRING("rule_latched", protocolErrorName(PROTOCOL_ERR_RULE_LATCHED));
}

// ------------------------------------------------------------ config blob

static const LampControlConfig kLamp = {40.0f, 120.0f, 30000, 30000, 5};
static const LinearCalibration kPh = {-0.0057f, 22.7f, 0, false};
static const LinearCalibration kTds = {1.0f, 0.0f, 1, true};
static const LinearCalibration kLight = {1.0f, 0.0f, 0, false};

static ConfigDefaults defaults(void) {
  ConfigDefaults d;
  memset(&d, 0, sizeof(d));
  d.lampAutoEnabled = true;
  d.lamp = kLamp;
  d.ph = kPh;
  d.tds = kTds;
  d.light = kLight;
  return d;
}

// Audit P2: a long integer must not overflow the accumulator (signed
// overflow is undefined), in a known field or in an ignored one.
static void test_parse_long_integers_without_overflow(void) {
  Command c;
  TEST_ASSERT_EQUAL(PROTOCOL_ERR_BAD_FIELD,
                    parse("{\"v\":2,\"id\":\"a\",\"op\":\"reset_latch\","
                          "\"data\":{\"rule\":123456789012345678901234567890}}", &c));
  TEST_ASSERT_EQUAL(PROTOCOL_OK,
                    parse("{\"v\":2,\"id\":\"a\",\"op\":\"reset_latch\",\"pad\":-999999999999999999999999999999,"
                          "\"data\":{\"rule\":1}}", &c));
  TEST_ASSERT_EQUAL_UINT8(1, c.rule);
}

// A stored fit that is not a finite number is a corrupt blob.
static void test_blob_rejects_non_finite_fit(void) {
  const ConfigDefaults d = defaults();
  ConfigBlob b;
  configBlobInit(&b, &d);
  b.ph.slope = NAN;
  configBlobSeal(&b);
  TEST_ASSERT_FALSE(configBlobValid(&b));
}

static void test_blob_defaults_seal_and_corruption(void) {
  const ConfigDefaults d = defaults();
  ConfigBlob b;
  configBlobInit(&b, &d);
  TEST_ASSERT_TRUE(configBlobValid(&b));
  TEST_ASSERT_EQUAL_UINT16(1, b.revision);
  TEST_ASSERT_EQUAL_UINT16(2, b.schema);
  TEST_ASSERT_EQUAL_HEX8(SENSOR_MASK_ALL, b.sensorEnabled);
  TEST_ASSERT_EQUAL_HEX8(0, b.calOverridden);
  ConfigBlob bad;
  memcpy(&bad, &b, sizeof(bad));
  ((uint8_t*)&bad)[12] ^= 0x01;  // one flipped bit in the lamp policy
  TEST_ASSERT_FALSE(configBlobValid(&bad));
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32Ieee("123456789", 9));  // standard check value
}

static void test_blob_sensor_change_and_slot_pick(void) {
  const ConfigDefaults d = defaults();
  ConfigBlob a, b;
  configBlobInit(&a, &d);
  TEST_ASSERT_FALSE(configBlobWithSensor(&a, SENSOR_TDS, true, &b));  // no change
  TEST_ASSERT_TRUE(configBlobWithSensor(&a, SENSOR_TDS, false, &b));
  TEST_ASSERT_TRUE(configBlobValid(&b));
  TEST_ASSERT_EQUAL_UINT16(2, b.revision);
  TEST_ASSERT_FALSE(sensorMaskHas(b.sensorEnabled, SENSOR_TDS));
  TEST_ASSERT_TRUE(configBlobPickNewest(&a, &b) == &b);
  TEST_ASSERT_TRUE(configBlobPickNewest(&b, &a) == &b);
  b.crc ^= 1u;  // newest slot torn: fall back to the older one
  TEST_ASSERT_TRUE(configBlobPickNewest(&a, &b) == &a);
  TEST_ASSERT_NULL(configBlobPickNewest(NULL, &b));
  // Revision wrap: 0xFFFF -> 1 is newer.
  a.revision = 0xFFFF;
  configBlobSeal(&a);
  TEST_ASSERT_TRUE(configBlobWithSensor(&a, SENSOR_PH, false, &b));
  TEST_ASSERT_EQUAL_UINT16(1, b.revision);
  TEST_ASSERT_TRUE(configBlobPickNewest(&a, &b) == &b);
}

static void test_blob_refresh_defaults_follows_compiled_values(void) {
  const ConfigDefaults d = defaults();
  ConfigBlob b;
  configBlobInit(&b, &d);
  ConfigBlob tdsOff;
  TEST_ASSERT_TRUE(configBlobWithSensor(&b, SENSOR_TDS, false, &tdsOff));
  // Same compiled values: untouched.
  TEST_ASSERT_FALSE(configBlobRefreshDefaults(&tdsOff, &d));
  TEST_ASSERT_EQUAL_UINT16(2, tdsOff.revision);
  // A re-measured pH offset in the source wins over the stored copy, keeps the
  // sensor mask, and gets a new revision.
  ConfigDefaults d2 = d;
  d2.ph.offset = 23.1f;
  TEST_ASSERT_TRUE(configBlobRefreshDefaults(&tdsOff, &d2));
  TEST_ASSERT_EQUAL_UINT16(3, tdsOff.revision);
  TEST_ASSERT_EQUAL_FLOAT(23.1f, tdsOff.ph.offset);
  TEST_ASSERT_FALSE(sensorMaskHas(tdsOff.sensorEnabled, SENSOR_TDS));
  TEST_ASSERT_TRUE(configBlobValid(&tdsOff));
}

static void test_blob_user_calibration_survives_refresh_and_reset(void) {
  const ConfigDefaults d = defaults();
  ConfigBlob b, cal, reset;
  configBlobInit(&b, &d);
  const LinearCalibration fit = {-0.0061f, 23.9f, 0, true};
  TEST_ASSERT_TRUE(configBlobWithCalibration(&b, SENSOR_PH, &fit, true, &cal));
  TEST_ASSERT_TRUE(configBlobValid(&cal));
  TEST_ASSERT_EQUAL_UINT16(2, cal.revision);
  TEST_ASSERT_EQUAL_UINT16(2, cal.ph.revision);  // cal_rev = config revision of the fit
  TEST_ASSERT_EQUAL_HEX8(CAL_BIT_PH, cal.calOverridden);
  // A changed compiled pH default does NOT replace the user fit...
  ConfigDefaults d2 = d;
  d2.ph.offset = 99.0f;
  d2.light.slope = 2.0f;
  TEST_ASSERT_TRUE(configBlobRefreshDefaults(&cal, &d2));  // ...but the light default does change
  TEST_ASSERT_EQUAL_FLOAT(23.9f, cal.ph.offset);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, cal.light.slope);
  // Reset: compiled default back, bit cleared, refreshed from then on.
  TEST_ASSERT_TRUE(configBlobWithCalibration(&cal, SENSOR_PH, &d2.ph, false, &reset));
  TEST_ASSERT_EQUAL_HEX8(0, reset.calOverridden);
  TEST_ASSERT_EQUAL_FLOAT(99.0f, reset.ph.offset);
  TEST_ASSERT_EQUAL_UINT16(0, reset.ph.revision);
  TEST_ASSERT_FALSE(configBlobWithCalibration(&cal, SENSOR_BME280, &fit, true, &reset));
  TEST_ASSERT_TRUE(configBlobCalibration(&cal, SENSOR_LIGHT) == &cal.light);
  TEST_ASSERT_NULL(configBlobCalibration(&cal, SENSOR_SGP30));
}

static void test_blob_migrates_schema_1(void) {
  ConfigBlobV1 old;
  memset(&old, 0, sizeof(old));
  old.magic = CONFIG_BLOB_MAGIC;
  old.schema = 1;
  old.revision = 7;
  old.sensorEnabled = (SensorMask)(SENSOR_MASK_ALL & ~(1u << SENSOR_TDS));
  old.lampAutoEnabled = true;
  old.lamp = kLamp;
  old.ph = kPh;
  old.tds = kTds;
  old.crc = crc32Ieee(&old, (uint32_t)offsetof(ConfigBlobV1, crc));
  const ConfigDefaults d = defaults();
  ConfigBlob b;
  TEST_ASSERT_TRUE(configBlobMigrateV1(&old, sizeof(old), &d, &b));
  TEST_ASSERT_TRUE(configBlobValid(&b));
  TEST_ASSERT_EQUAL_UINT16(7, b.revision);
  TEST_ASSERT_EQUAL_HEX8(old.sensorEnabled, b.sensorEnabled);  // the user's choice survives the update
  TEST_ASSERT_EQUAL_HEX8(0, b.calOverridden);
  TEST_ASSERT_FALSE(b.light.valid);
  old.crc ^= 1u;
  TEST_ASSERT_FALSE(configBlobMigrateV1(&old, sizeof(old), &d, &b));
  TEST_ASSERT_FALSE(configBlobMigrateV1(&old, sizeof(old) - 1, &d, &b));
  TEST_ASSERT_TRUE(sizeof(ConfigBlob) != sizeof(ConfigBlobV1));  // so getBlob() can tell them apart
}

// ------------------------------------------------------------ calibration fits / capture

static void test_calibration_fits(void) {
  LinearCalibration c;
  // Buffers 7.00 at 2500 mV and 4.00 at 3030 mV: slope -0.00566 pH/mV.
  TEST_ASSERT_TRUE(phFitFromBuffers(2500.0f, 7.0f, 3030.0f, 4.0f, 0.002f, 0.02f, 5, &c));
  TEST_ASSERT_TRUE(c.valid);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 7.0f, linearApply(&c, 2500.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 4.0f, linearApply(&c, 3030.0f));
  TEST_ASSERT_FALSE(phFitFromBuffers(2500.0f, 7.0f, 2510.0f, 4.0f, 0.002f, 0.02f, 5, &c));  // too steep
  TEST_ASSERT_FALSE(c.valid);
  TEST_ASSERT_FALSE(phFitFromBuffers(2500.0f, 7.0f, 2600.0f, 6.5f, 0.002f, 0.02f, 5, &c));  // < 1 pH apart
  TEST_ASSERT_FALSE(phFitFromBuffers(2500.0f, 7.0f, 2500.0f, 4.0f, 0.002f, 0.02f, 5, &c));  // same mV
  // Buffer values swapped: slope +0.00566 pH/mV, plausible size, wrong sign.
  TEST_ASSERT_FALSE(phFitFromBuffers(2500.0f, 4.0f, 3030.0f, 7.0f, 0.002f, 0.02f, 5, &c));
  TEST_ASSERT_FALSE(c.valid);
  // Same buffers entered in the other order: still a valid (negative) fit.
  TEST_ASSERT_TRUE(phFitFromBuffers(3030.0f, 4.0f, 2500.0f, 7.0f, 0.002f, 0.02f, 5, &c));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 7.0f, linearApply(&c, 2500.0f));
  // TDS K: 650 ppm nominal in a 707 ppm solution.
  TEST_ASSERT_TRUE(linearGainFromPoint(650.0f, 707.0f, 0.5f, 2.0f, 0, &c));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 707.0f / 650.0f, c.slope);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.offset);
  TEST_ASSERT_FALSE(linearGainFromPoint(100.0f, 707.0f, 0.5f, 2.0f, 0, &c));  // K = 7
  TEST_ASSERT_FALSE(linearGainFromPoint(0.0f, 707.0f, 0.5f, 2.0f, 0, &c));
  // Light: 1200 mV under a 3000 lux reference.
  TEST_ASSERT_TRUE(linearGainFromPoint(1200.0f, 3000.0f, 0.01f, 100.0f, 0, &c));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 3000.0f, linearApply(&c, 1200.0f));
}

static Reading reading(SensorId s, MetricId m, Unit u, Health h, float value, int32_t raw, MonoMs end) {
  Reading r;
  readingReset(&r, s, m, u);
  r.health = h;
  r.value = value;
  r.hasRaw = true;
  r.raw = raw;
  r.intervalEndMs = end;
  return r;
}

static void test_capture_from_snapshot(void) {
  Snapshot snap;
  snapshotReset(&snap);
  const CaptureLimits lim = {5000, 30000, 10000, 15000};
  float x;
  uint32_t age;
  // Nothing measured yet.
  TEST_ASSERT_EQUAL(CAPTURE_STALE, calCaptureFromSnapshot(SENSOR_PH, &snap, 10000, &lim, &x, &age));
  // pH: pin 1500 mV through 10k/15k -> module 2500 mV.
  snap.readings[METRIC_PH] = reading(SENSOR_PH, METRIC_PH, UNIT_PH, HEALTH_UNCALIBRATED, 7.0f, 1500, 9000);
  TEST_ASSERT_EQUAL(CAPTURE_OK, calCaptureFromSnapshot(SENSOR_PH, &snap, 10000, &lim, &x, &age));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2500.0f, x);
  TEST_ASSERT_EQUAL_UINT32(1000, age);
  TEST_ASSERT_EQUAL(CAPTURE_STALE, calCaptureFromSnapshot(SENSOR_PH, &snap, 20000, &lim, &x, &age));  // too old
  snap.readings[METRIC_PH].health = HEALTH_UNPOWERED;
  TEST_ASSERT_EQUAL(CAPTURE_STALE, calCaptureFromSnapshot(SENSOR_PH, &snap, 10000, &lim, &x, &age));
  // Light: raw pin mV even after a lux fit.
  snap.readings[METRIC_LIGHT_LEVEL] =
      reading(SENSOR_LIGHT, METRIC_LIGHT_LEVEL, UNIT_LUX, HEALTH_OK, 3000.0f, 1200, 9500);
  TEST_ASSERT_EQUAL(CAPTURE_OK, calCaptureFromSnapshot(SENSOR_LIGHT, &snap, 10000, &lim, &x, &age));
  TEST_ASSERT_EQUAL_FLOAT(1200.0f, x);
  // TDS: refused without a usable water temperature, then the nominal ppm at 25 degC.
  snap.readings[METRIC_TDS] = reading(SENSOR_TDS, METRIC_TDS, UNIT_PPM, HEALTH_UNCALIBRATED, 0.0f, 1000, 9500);
  TEST_ASSERT_EQUAL(CAPTURE_NO_TEMPERATURE, calCaptureFromSnapshot(SENSOR_TDS, &snap, 10000, &lim, &x, &age));
  snap.readings[METRIC_WATER_TEMPERATURE] =
      reading(SENSOR_DS18B20, METRIC_WATER_TEMPERATURE, UNIT_DEG_C, HEALTH_OK, 25.0f, 400, 9000);
  TEST_ASSERT_EQUAL(CAPTURE_OK, calCaptureFromSnapshot(SENSOR_TDS, &snap, 10000, &lim, &x, &age));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, (133.42f - 255.86f + 857.39f) * 0.5f, x);  // 1 V on the curve
  TEST_ASSERT_EQUAL(CAPTURE_UNSUPPORTED, calCaptureFromSnapshot(SENSOR_BME280, &snap, 10000, &lim, &x, &age));
}

// ------------------------------------------------------------ journal

static EventJournal s_journal;

static void test_journal_cursors_are_independent(void) {
  journalInit(&s_journal);
  JournalCursor ble, lan;
  journalCursorInit(&s_journal, &ble);
  journalCursorInit(&s_journal, &lan);
  JournalEvent e = {0};
  for (uint8_t i = 0; i < 3; ++i) {
    e.ruleId = i;
    journalAppend(&s_journal, &e);
  }
  JournalEvent out;
  bool gap;
  TEST_ASSERT_TRUE(journalNext(&s_journal, &ble, &out, &gap));
  TEST_ASSERT_EQUAL_UINT32(1, out.n);
  TEST_ASSERT_FALSE(gap);
  for (uint32_t n = 1; n <= 3; ++n) {
    TEST_ASSERT_TRUE(journalNext(&s_journal, &lan, &out, &gap));
    TEST_ASSERT_EQUAL_UINT32(n, out.n);
  }
  TEST_ASSERT_FALSE(journalNext(&s_journal, &lan, &out, &gap));
  TEST_ASSERT_TRUE(journalNext(&s_journal, &ble, &out, &gap));
  TEST_ASSERT_EQUAL_UINT32(2, out.n);
}

static void test_journal_overrun_reports_gap(void) {
  journalInit(&s_journal);
  JournalCursor c;
  journalCursorInit(&s_journal, &c);
  JournalEvent e = {0};
  for (int i = 0; i < EVENT_JOURNAL_LEN + 10; ++i) journalAppend(&s_journal, &e);
  JournalEvent out;
  bool gap;
  TEST_ASSERT_TRUE(journalNext(&s_journal, &c, &out, &gap));
  TEST_ASSERT_TRUE(gap);
  TEST_ASSERT_EQUAL_UINT32(11, out.n);  // oldest still stored
  TEST_ASSERT_TRUE(journalNext(&s_journal, &c, &out, &gap));
  TEST_ASSERT_FALSE(gap);
  TEST_ASSERT_EQUAL_UINT32(12, out.n);
}

// ------------------------------------------------------------ backoff

static void test_backoff_doubles_with_bounded_jitter(void) {
  Backoff b;
  backoffInit(&b, 1000, 60000, 20);
  const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000};
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    const uint32_t base = b.currentMs;
    TEST_ASSERT_EQUAL_UINT32(expected[i], base);
    const uint32_t lo = backoffNext(&b, 0);
    b.currentMs = base;
    const uint32_t hi = backoffNext(&b, 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(lo >= base - base / 5);
    TEST_ASSERT_TRUE(hi <= base + base / 5);
    TEST_ASSERT_TRUE(hi <= 60000);
  }
  backoffReset(&b);
  TEST_ASSERT_EQUAL_UINT32(1000, b.currentMs);
}

// ------------------------------------------------------------ safety

static const SafetyConfig kSafety = {true, {40.0f, 120.0f, 0, 0, 5}, 3000};
static const AlarmRule kRules[2] = {
    {.ruleId = 0, .enabled = true, .metric = METRIC_AIR_TEMPERATURE, .comparison = COMPARISON_ABOVE,
     .tripThreshold = 40.0f, .clearThreshold = 37.0f, .persistMs = 0, .severity = SEVERITY_CRITICAL,
     .latching = true, .actuatorId = ACTUATOR_RELAY1, .action = ACTUATOR_ACTION_DEENERGIZE},
    {.ruleId = 1, .enabled = true, .metric = METRIC_TDS, .comparison = COMPARISON_ABOVE,
     .tripThreshold = 1500.0f, .clearThreshold = 1400.0f, .persistMs = 0, .severity = SEVERITY_WARNING,
     .latching = false, .actuatorId = ACTUATOR_NONE, .action = ACTUATOR_ACTION_NONE},
};

static SafetyCore s_core;

static AlarmEvent airEvent(RuleDecision d, float v, MonoMs end) {
  AlarmEvent e;
  memset(&e, 0, sizeof(e));
  e.ruleId = 0;
  e.decision = d;
  readingReset(&e.sample, SENSOR_BME280, METRIC_AIR_TEMPERATURE, UNIT_DEG_C);
  e.sample.health = HEALTH_OK;
  e.sample.value = v;
  e.sample.intervalEndMs = end;
  return e;
}

static void test_safety_reset_latch_command(void) {
  safetyCoreInit(&s_core, &kSafety, kRules, 2, 0);
  snapshotReset(&s_snap);
  SafetyOutputs out;
  const AlarmEvent trip = airEvent(RULE_DECISION_TRIP, 45.0f, 100);
  SafetyInputs in = {&s_snap, &trip, 1, 100};
  safetyCoreStep(&s_core, &in, &out);
  TEST_ASSERT_EQUAL_HEX16(0x0001, out.latchedMask);

  // Still active: refused.
  const SafetyCommand reset = {SAFETY_CMD_RESET_LATCH, 0, 5000, 77};
  SafetyInputs cmd = {&s_snap, NULL, 0, 200, &reset, 1, 0};
  safetyCoreStep(&s_core, &cmd, &out);
  TEST_ASSERT_EQUAL(1, out.resultCount);
  TEST_ASSERT_EQUAL_UINT32(77, out.results[0].tag);
  TEST_ASSERT_EQUAL(SAFETY_CMD_STILL_ACTIVE, out.results[0].outcome);
  TEST_ASSERT_TRUE(out.interlock);

  // Recovery sample clears "active"; an expired reset is still refused.
  const AlarmEvent clear = airEvent(RULE_DECISION_CLEAR, 30.0f, 300);
  SafetyInputs rec = {&s_snap, &clear, 1, 300};
  safetyCoreStep(&s_core, &rec, &out);
  TEST_ASSERT_EQUAL_HEX16(0x0001, out.latchedMask);
  SafetyInputs late = {&s_snap, NULL, 0, 6000, &reset, 1, 0};
  safetyCoreStep(&s_core, &late, &out);
  TEST_ASSERT_EQUAL(SAFETY_CMD_EXPIRED, out.results[0].outcome);
  TEST_ASSERT_EQUAL_HEX16(0x0001, out.latchedMask);

  const SafetyCommand fresh = {SAFETY_CMD_RESET_LATCH, 0, 9000, 78};
  SafetyInputs ok = {&s_snap, NULL, 0, 6100, &fresh, 1, 0};
  safetyCoreStep(&s_core, &ok, &out);
  TEST_ASSERT_EQUAL(SAFETY_CMD_APPLIED, out.results[0].outcome);
  TEST_ASSERT_EQUAL_HEX16(0x0000, out.latchedMask);
  TEST_ASSERT_FALSE(out.interlock);

  const SafetyCommand bad = {SAFETY_CMD_RESET_LATCH, 5, 9000, 79};
  SafetyInputs b = {&s_snap, NULL, 0, 6200, &bad, 1, 0};
  safetyCoreStep(&s_core, &b, &out);
  TEST_ASSERT_EQUAL(SAFETY_CMD_BAD_RULE, out.results[0].outcome);
}

static void test_safety_disabled_sensor_cannot_silence_alarm(void) {
  // A latched or active rule blocks disabling its sensor.
  TEST_ASSERT_TRUE(safetyRulesBlockSensor(kRules, 2, 0x0001, 0, SENSOR_BME280));
  TEST_ASSERT_TRUE(safetyRulesBlockSensor(kRules, 2, 0, 0x0002, SENSOR_TDS));
  TEST_ASSERT_FALSE(safetyRulesBlockSensor(kRules, 2, 0x0001, 0, SENSOR_TDS));
  TEST_ASSERT_FALSE(safetyRulesBlockSensor(kRules, 2, 0, 0, SENSOR_BME280));

  // Disabled and unlatched: no stale report. Latched: still reported, and the
  // interlock holds, even with the input switched off.
  safetyCoreInit(&s_core, &kSafety, kRules, 2, 0);
  snapshotReset(&s_snap);
  SafetyOutputs out;
  const SensorMask off = (SensorMask)((1u << SENSOR_BME280) | (1u << SENSOR_TDS));
  SafetyInputs quiet = {&s_snap, NULL, 0, 100, NULL, 0, off};
  safetyCoreStep(&s_core, &quiet, &out);
  TEST_ASSERT_EQUAL_HEX16(0x0000, out.staleMask);

  const AlarmEvent trip = airEvent(RULE_DECISION_TRIP, 45.0f, 200);
  SafetyInputs t = {&s_snap, &trip, 1, 200, NULL, 0, off};
  safetyCoreStep(&s_core, &t, &out);
  SafetyInputs later = {&s_snap, NULL, 0, 5000, NULL, 0, off};
  safetyCoreStep(&s_core, &later, &out);
  TEST_ASSERT_EQUAL_HEX16(0x0001, out.staleMask);
  TEST_ASSERT_EQUAL_HEX16(0x0001, out.latchedMask);
  TEST_ASSERT_TRUE(out.interlock);
  TEST_ASSERT_FALSE(out.relayOn);
}

static void test_sensor_keys_round_trip(void) {
  for (uint8_t s = 0; s < SENSOR_COUNT; ++s) {
    const char* k = sensorKey((SensorId)s);
    TEST_ASSERT_EQUAL(s, sensorFromKey(k, strlen(k)));
  }
  TEST_ASSERT_EQUAL(SENSOR_COUNT, sensorFromKey("ph2", 3));
  TEST_ASSERT_EQUAL(SENSOR_SGP30, sensorOfMetric(METRIC_CO2EQ));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_frame_split_across_packets);
  RUN_TEST(test_frame_two_in_one_packet);
  RUN_TEST(test_frame_rejects_zero_and_oversize);
  RUN_TEST(test_telemetry_value_rules);
  RUN_TEST(test_telemetry_overflow_returns_zero);
  RUN_TEST(test_state_escapes_and_hides_ip_unless_connected);
  RUN_TEST(test_results_and_sensor_list);
  RUN_TEST(test_parse_valid_commands);
  RUN_TEST(test_parse_calibration_commands);
  RUN_TEST(test_encode_calibration_and_capture);
  RUN_TEST(test_encode_diagnostics);
  RUN_TEST(test_parse_rejects_malformed_and_out_of_range);
  RUN_TEST(test_parse_keeps_id_for_error_reply);
  RUN_TEST(test_blob_defaults_seal_and_corruption);
  RUN_TEST(test_blob_sensor_change_and_slot_pick);
  RUN_TEST(test_blob_refresh_defaults_follows_compiled_values);
  RUN_TEST(test_blob_user_calibration_survives_refresh_and_reset);
  RUN_TEST(test_blob_migrates_schema_1);
  RUN_TEST(test_calibration_fits);
  RUN_TEST(test_capture_from_snapshot);
  RUN_TEST(test_journal_cursors_are_independent);
  RUN_TEST(test_journal_overrun_reports_gap);
  RUN_TEST(test_backoff_doubles_with_bounded_jitter);
  RUN_TEST(test_safety_reset_latch_command);
  RUN_TEST(test_safety_disabled_sensor_cannot_silence_alarm);
  RUN_TEST(test_sensor_keys_round_trip);
  RUN_TEST(test_parse_long_integers_without_overflow);
  RUN_TEST(test_blob_rejects_non_finite_fit);
  return UNITY_END();
}

// Configuration: validated commands, versioned config, sole NVS writer.
// Work package WP8 (Stage 5). Architecture §17; RTOS §14.1.
//
// NVS namespace "hydro":
//   cfg_a / cfg_b  ConfigBlob A/B slots; boot loads the newest valid one
//                  (schema 1 slots are migrated: the sensor mask survives)
//   wifi           ConfigWifi (credentials; never logged, never sent back)
//   sgp_base       SGP30 baseline (co2eq, tvoc), written at most hourly
//   rst_hist       last 8 reset reasons, written once ~30 s after boot
//
// configurationInit() loads and PUBLISHES the applied configuration before it
// returns, so Safety and acquisition (started next) read it on their first
// pass. If NVS fails, the compiled defaults (all sensors on) are published.
//
// The LAN link and its "lan_tok" key are gone (the app link is Bluetooth
// only); a left-over lan_tok is erased once at boot.
//
// Calibration (Calibration page): set_calibration fits the points sent by the
// app (domain/calibration), refuses an implausible fit (bad_fit) or a sensor
// whose alarm rule is latched / active, stores the fit with its CAL_BIT_* set
// so the boot refresh keeps it, and waits until acquisition has applied it.
// reset_calibration restores the compiled default and clears the bit.
//
// Requests come through an 8-entry queue (CFG_COMMAND_QUEUE_LEN) from the
// transports; a full queue is reported as busy, never as success (§11). The
// Configuration task checks expiry and session before touching flash, writes
// the inactive slot, publishes, waits (bounded) until acquisition has applied
// the new revision, then replies with the actual outcome.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/config_blob.h"
#include "domain/protocol_codec.h"
#include "domain/types.h"

typedef uint8_t ConfigRequestKind;
enum {
  CONFIG_REQ_SET_SENSOR_ENABLED = 0,
  CONFIG_REQ_SET_WIFI,
  CONFIG_REQ_FORGET_WIFI,
  CONFIG_REQ_SET_CALIBRATION,
  CONFIG_REQ_RESET_CALIBRATION,
};

typedef struct ConfigResult {
  uint32_t tag;
  ProtocolError error;              // PROTOCOL_OK on success
  uint16_t cfgRev;                  // sensor / calibration requests: the applied revision
} ConfigResult;

// Called from the Configuration task: copy the result and return at once.
typedef void (*ConfigReplyFn)(const ConfigResult* result, void* ctx);

typedef struct ConfigRequest {
  ConfigRequestKind kind;
  uint32_t tag;            // caller's handle, echoed in the result
  uint32_t session;        // transport session generation at receive time
  uint32_t (*sessionNow)(void);  // current generation; NULL = no session check
  MonoMs expiresMs;        // not processed after this
  ConfigReplyFn reply;
  void* replyCtx;
  SensorId sensor;
  bool enabled;
  char ssid[PROTOCOL_SSID_MAX + 1];
  char password[PROTOCOL_PASSWORD_MAX + 1];
  int32_t calX1, calY1, calX2, calY2;  // SET_CALIBRATION, scaled as in protocol_codec.h
} ConfigRequest;

typedef struct ConfigWifi {
  char ssid[PROTOCOL_SSID_MAX + 1];
  char password[PROTOCOL_PASSWORD_MAX + 1];
} ConfigWifi;

bool configurationInit(void);

// Zero wait. false = queue full (reply "busy") or not initialised.
bool configurationSubmit(const ConfigRequest* request);

// Latest applied configuration (zero wait). Before init or on failure it
// fills the compiled defaults and returns false.
bool configurationPeekApplied(ConfigBlob* out);

// Stored SGP30 baseline from the last boot, for acquisition before init.
bool configurationStoredBaseline(uint16_t* co2eq, uint16_t* tvoc);

// Acquisition offers the current baseline (zero wait); written at most hourly
// and only when it changed.
void configurationOfferBaseline(uint16_t co2eq, uint16_t tvoc);

// Acquisition reports the revision it applied at a frame boundary.
void configurationAckApplied(uint16_t revision);

// Wi-Fi credentials for the Wi-Fi owner (Stage 8.4). changeCount increases on
// every set/forget, so the reader knows when to reconnect.
bool configurationGetWifi(ConfigWifi* out, uint32_t* changeCount);

// For Supervisor. NULL before init.
void* configurationTaskHandle(void);

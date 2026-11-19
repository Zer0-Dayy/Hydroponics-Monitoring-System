// State / Telemetry / event frames for the BLE link (WP8).
// FirmwareContract.md v2. The app keeps one row per (node, boot, metric, seq).
//
// Each calling task passes its own scratch (no shared buffers, no locks);
// every input is a zero-wait peek (snapshot, SafetyStatus, applied config,
// Wi-Fi status). Encoders write the JSON payload only (no length header).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/config_blob.h"
#include "domain/event_journal.h"
#include "domain/reading.h"
#include "services/configuration.h"

#define TRANSPORT_NODE_NAME "Hydro-ESP1"

typedef struct TransportScratch {
  Snapshot snap;
  ConfigBlob cfg;
  char ssid[PROTOCOL_SSID_MAX + 1];
  char ip[16];
} TransportScratch;

// Random per boot, drawn on first use (after the radio starts: true RNG).
uint32_t transportBootId(void);

// link: "ble" (the LAN link was removed after Stage 8.4).
size_t transportEncodeState(TransportScratch* s, const char* link, char* buf, size_t cap);

// 0 if no snapshot has been published yet.
size_t transportEncodeTelemetry(TransportScratch* s, char* buf, size_t cap);

size_t transportEncodeEvent(const JournalEvent* e, bool gap, char* buf, size_t cap);

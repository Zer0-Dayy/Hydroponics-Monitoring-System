// BleTransport: the one application GATT sender (Architecture §17).
// Work package WP8 (Stage 8.3). FirmwareContract.md v2.
//
// Task "BleTransport" (cfgTaskSpec: core 0, priority 2, 6 KB). It owns the
// GATT shim (ble_gatt.h), reassembles command frames, routes them (get_sensors
// here, and get_calibration / cal_capture from zero-wait peeks;
// set_sensor_enabled / set_wifi / forget_wifi / set_calibration /
// reset_calibration to Configuration; reset_latch to Safety) and sends:
//   results and alarm events first, then State (every 2 s and on link-up),
//   then Telemetry (every 3 s). A telemetry frame that cannot be queued is
//   dropped and superseded by the next one; results are retried.
// It peeks the snapshot, SafetyStatus and the applied config with zero wait
// and never calls a driver. Requests carry the link's session generation and
// an expiry: a reply is never sent on a newer link, a command never applied
// late.
//
// Link: open (no pairing, no bonding, no passkey), one client at a time.
// Data is sent only once the client has subscribed to State, Telemetry and
// Result; a client still not subscribed 20 s after connecting is dropped so
// it cannot hold the only slot.
//
// Built only with -D FW_ENABLE_BLE (production envs). Without it
// bleTransportInit() returns false and nothing links NimBLE.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool bleTransportInit(void);

typedef struct BleTransportStats {
  uint32_t framesSent;
  uint32_t telemetryDropped;   // superseded: link congested
  uint32_t resultsDropped;     // link closed or buffers stayed full
  uint32_t commands;
  uint32_t commandErrors;      // malformed frames / fields
  uint32_t busy;               // replies "busy" (queues full)
  uint32_t rxOverflow;         // command bytes lost before parsing
  uint32_t maxSendMs;          // longest frame send (all chunks)
} BleTransportStats;

void bleTransportGetStats(BleTransportStats* out);

// For Supervisor. NULL before init or without FW_ENABLE_BLE.
void* bleTransportTaskHandle(void);

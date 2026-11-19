// GATT server shim over NimBLE-Arduino 2.5.1 (C++), callable from C.
// Work package WP8 (Stage 5). Architecture §17; FirmwareContract.md v2.
//
// The shim is the only code that touches NimBLE. It owns the service map,
// the one-client rule and the link "session generation". It does no protocol
// work: received Command bytes go to one callback, and the caller notifies
// already-framed bytes one chunk at a time.
//
// Open link (since 0.11.0): no pairing, no bonding, no passkey. Any client
// in range can connect, one at a time. Bonds left in NVS by older firmware
// are deleted at start, so no stale key can break a later connection.
//
// Threading: the RX callback runs in the NimBLE host task (core 0). It must
// only copy the bytes and return (e.g. xStreamBufferSend with zero wait).
// All other functions may be called from one application task
// (BleTransport). A channel is notified only once the client has subscribed
// to it, so a frame never starts before the client listens.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t BleChannel;
enum { BLE_CHANNEL_STATE = 0, BLE_CHANNEL_TELEMETRY, BLE_CHANNEL_RESULT, BLE_CHANNEL_COUNT };

// Command characteristic write, raw bytes as received (any chunking).
typedef void (*BleRxFn)(const uint8_t* data, size_t len, uint32_t generation, void* ctx);

typedef struct BleGattStats {
  uint32_t connects;
  uint32_t disconnects;
  uint32_t refused;            // a second client while one was connected
  uint32_t idleDropped;        // connected but never subscribed (bleGattDropIdle)
  uint32_t notifyFailures;     // chunks NimBLE could not queue
  int lastDisconnectReason;
} BleGattStats;

// Starts NimBLE, the GATT service and advertising. Returns false if NimBLE
// failed to start.
bool bleGattStart(const char* deviceName, BleRxFn rx, void* rxCtx);

// True while a client is connected.
bool bleGattConnected(void);

// True while the connected client has subscribed to all three notify
// channels (State, Telemetry, Result): the app is listening.
bool bleGattReady(void);

// Incremented on every connect and disconnect: requests from an earlier
// session must not be answered on a newer one.
uint32_t bleGattGeneration(void);

// Largest notification payload on the current link (ATT MTU - 3), >= 20.
size_t bleGattChunkSize(void);

// Sends one notification of at most bleGattChunkSize() bytes. false if no
// client is connected, it has not subscribed to this channel, the chunk is
// too big, or NimBLE is out of buffers (the caller may retry shortly or drop
// the frame).
bool bleGattNotify(BleChannel channel, const uint8_t* data, size_t len);

// Value returned by a read of the State characteristic (one whole frame,
// header included; long reads are supported up to BLE_GATT_STATE_MAX).
#define BLE_GATT_STATE_MAX 1024
bool bleGattSetStateValue(const uint8_t* frame, size_t len);

// Drops the current client. Used when a notification frame was cut after
// its first chunk: the client's stream is out of step.
void bleGattDisconnect(void);

// Drops a client that connected but never subscribed (an operating system
// holding the link without an app on it) so the slot is free for the next
// client. Counted in idleDropped.
void bleGattDropIdle(void);

void bleGattGetStats(BleGattStats* out);

#ifdef __cplusplus
}
#endif

// Length-prefixed frame reassembly (FirmwareContract.md v2, §17). Pure C.
// Work package WP8 (Stage 5). Shared by the BLE and LAN transports.
//
// Every frame is a 2-byte little-endian payload length followed by that many
// bytes of UTF-8 JSON. Transport packets may split or merge frames, so the
// receiver feeds whatever arrives and takes complete payloads out one by one.
// A length of 0 or above FRAME_MAX_PAYLOAD is a protocol error: the buffer is
// dropped (the caller decides whether to disconnect).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_HEADER_BYTES ((size_t)2)
#define FRAME_MAX_PAYLOAD ((size_t)4096)

typedef uint8_t FrameStatus;
enum {
  FRAME_NEED_MORE = 0,  // all input consumed, no complete frame yet
  FRAME_READY,          // one payload available; feed the rest of the input again
  FRAME_ERROR           // bad length; assembler reset
};

typedef struct FrameAssembler {
  uint8_t buf[FRAME_HEADER_BYTES + FRAME_MAX_PAYLOAD];
  size_t used;
  bool ready;          // buf holds a complete frame returned by the last feed
  uint32_t rejected;   // FRAME_ERROR count
} FrameAssembler;

void frameAssemblerInit(FrameAssembler* self);

// Drop any partial frame (e.g. on disconnect). Keeps the rejected counter.
void frameAssemblerReset(FrameAssembler* self);

// Consume bytes from data[0..len) until one frame completes or the input ends.
// *consumed says how many bytes were used; call again with the remainder.
// On FRAME_READY, *payload / *payloadLen point into the assembler and stay
// valid until the next feed.
FrameStatus frameAssemblerFeed(FrameAssembler* self, const uint8_t* data, size_t len,
                               size_t* consumed, const uint8_t** payload, size_t* payloadLen);

// Write the 2-byte header for a payload of payloadLen bytes. false if the
// length is 0 or above FRAME_MAX_PAYLOAD.
bool frameWriteHeader(uint8_t out[2], size_t payloadLen);

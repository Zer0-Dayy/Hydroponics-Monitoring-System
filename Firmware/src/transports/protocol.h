// Protocol adapters: snapshot -> JSON state/telemetry frames.
// Work package WP8 (Stage 5). Architecture §17.
//
// The JSON itself is built in domain/protocol_codec (host-tested) and the
// frames in transports/frames; never from acquisition or ISRs. Coordinate
// schema changes with Protocol.py, Catalog.py and FirmwareContract.md in BluetoothControlApp/.
#pragma once
#include <stddef.h>

#define PROTOCOL_FRAME_HEADER_BYTES ((size_t)2)  // little-endian payload length
#define PROTOCOL_MAX_PAYLOAD_BYTES ((size_t)4096)

// Reported as "fw" in every State frame.
#define PROTOCOL_FIRMWARE_VERSION "0.11.0"

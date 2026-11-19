#include "domain/frame_codec.h"

#include <string.h>

void frameAssemblerInit(FrameAssembler* self) { memset(self, 0, sizeof(*self)); }

void frameAssemblerReset(FrameAssembler* self) {
  self->used = 0;
  self->ready = false;
}

static size_t declaredLength(const FrameAssembler* self) {
  return (size_t)self->buf[0] | ((size_t)self->buf[1] << 8);
}

FrameStatus frameAssemblerFeed(FrameAssembler* self, const uint8_t* data, size_t len,
                               size_t* consumed, const uint8_t** payload, size_t* payloadLen) {
  *consumed = 0;
  *payload = NULL;
  *payloadLen = 0;
  if (self->ready) frameAssemblerReset(self);  // the previous payload is released

  while (*consumed < len) {
    if (self->used < FRAME_HEADER_BYTES) {
      self->buf[self->used++] = data[(*consumed)++];
      if (self->used == FRAME_HEADER_BYTES) {
        const size_t declared = declaredLength(self);
        if (declared == 0 || declared > FRAME_MAX_PAYLOAD) {
          frameAssemblerReset(self);
          if (self->rejected < UINT32_MAX) ++self->rejected;
          return FRAME_ERROR;
        }
      }
      continue;
    }
    const size_t total = FRAME_HEADER_BYTES + declaredLength(self);
    size_t take = total - self->used;
    if (take > len - *consumed) take = len - *consumed;
    memcpy(&self->buf[self->used], &data[*consumed], take);
    self->used += take;
    *consumed += take;
    if (self->used == total) {
      self->ready = true;
      *payload = &self->buf[FRAME_HEADER_BYTES];
      *payloadLen = total - FRAME_HEADER_BYTES;
      return FRAME_READY;
    }
  }
  return FRAME_NEED_MORE;
}

bool frameWriteHeader(uint8_t out[2], size_t payloadLen) {
  if (payloadLen == 0 || payloadLen > FRAME_MAX_PAYLOAD) return false;
  out[0] = (uint8_t)(payloadLen & 0xFFu);
  out[1] = (uint8_t)(payloadLen >> 8);
  return true;
}

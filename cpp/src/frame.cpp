#include "frame.hpp"

#include <zlib.h>

#include <bit>
#include <cstdio>
#include <cstring>

namespace uti120 {

static uint32_t load_le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

Frame Frame::parse(std::span<const uint8_t> data) {
  char msg[96];
  if (data.size() != FRAME_BYTES) {
    std::snprintf(msg, sizeof msg, "frame has %zu bytes, expected %zu", data.size(), FRAME_BYTES);
    throw FrameError(msg);
  }
  uint32_t stored = load_le32(data.data() + FRAME_BYTES - 4);
  uint32_t crc = crc32(0L, data.data(), FRAME_BYTES - 4);
  if (crc != stored) {
    std::snprintf(msg, sizeof msg, "CRC mismatch: computed %08x, stored %08x", crc, stored);
    throw FrameError(msg);
  }
  static_assert(std::endian::native == std::endian::little, "frame words are little-endian");
  Frame f;
  std::memcpy(f.words.data(), data.data(), FRAME_BYTES);
  if (f.words[HDR_MAGIC] != MAGIC) {
    std::snprintf(msg, sizeof msg, "bad magic %04x", f.words[HDR_MAGIC]);
    throw FrameError(msg);
  }
  if (f.words[HDR_WIDTH] != WIDTH || f.words[HDR_HEIGHT] != SENSOR_ROWS) {
    std::snprintf(msg, sizeof msg, "unexpected geometry %ux%u", f.words[HDR_WIDTH],
                  f.words[HDR_HEIGHT]);
    throw FrameError(msg);
  }
  return f;
}

std::string Frame::describe() const {
  char buf[160];
  std::snprintf(buf, sizeof buf,
                "id=%d shutter=%s nuc=%d fpa=%.2fC shutter=%.2fC tube=%.2fC", frame_id(),
                shutter_closed() ? "closed" : "open", nuc_status(), fpa_temp(), shutter_temp(),
                tube_temp());
  return buf;
}

}  // namespace uti120

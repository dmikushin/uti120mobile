// Layout of one UTi120Mobile frame as delivered on the bulk endpoint.
//
// A frame is 25600 bytes of little-endian 16-bit words:
//
//     words [0, 120)                 header row (see the HDR_* indices below)
//     words [120, 120 + 120*92)      sensor block, 92 rows x 120 columns, 14-bit;
//                                    rows 0..1 are reference rows (always 0x3FFF),
//                                    rows 2..91 are the 90 active image rows
//     words [11160, 12798)           zero padding
//     bytes [25596, 25600)           CRC-32 (zlib polynomial) of bytes [0, 25596),
//                                    stored little-endian
//
// Header offsets are taken from FrameParamReader in the vendor Android app;
// temperatures are stored in hundredths of a degree Celsius.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace uti120 {

constexpr std::size_t FRAME_BYTES = 25600;
constexpr std::size_t FRAME_WORDS = FRAME_BYTES / 2;
constexpr int WIDTH = 120;
constexpr int HEIGHT = 90;
constexpr int PIXELS = WIDTH * HEIGHT;
constexpr int SENSOR_ROWS = 92;
constexpr int REFERENCE_ROWS = 2;
constexpr uint16_t MAGIC = 0xAA55;

constexpr int HDR_MAGIC = 0;
constexpr int HDR_FRAME_ID = 1;
constexpr int HDR_WIDTH = 2;
constexpr int HDR_HEIGHT = 3;
constexpr int HDR_SHUTTER_TEMP_INIT = 8;
constexpr int HDR_SHUTTER_TEMP = 9;
constexpr int HDR_TUBE_TEMP = 10;
constexpr int HDR_FPA_TEMP = 11;
constexpr int HDR_SHUTTER_CLOSED = 12;
constexpr int HDR_NUC_STATUS = 13;

struct FrameError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Frame {
  std::array<uint16_t, FRAME_WORDS> words{};

  // Validates length, CRC, magic and geometry; throws FrameError.
  static Frame parse(std::span<const uint8_t> data);

  // Active image, row-major, raw 14-bit detector counts.
  const uint16_t* pixels() const { return words.data() + WIDTH * (1 + REFERENCE_ROWS); }

  int frame_id() const { return words[HDR_FRAME_ID]; }
  bool shutter_closed() const { return words[HDR_SHUTTER_CLOSED] != 0; }
  int nuc_status() const { return words[HDR_NUC_STATUS]; }
  double shutter_temp() const { return words[HDR_SHUTTER_TEMP] / 100.0; }
  double shutter_temp_init() const { return words[HDR_SHUTTER_TEMP_INIT] / 100.0; }
  double tube_temp() const { return words[HDR_TUBE_TEMP] / 100.0; }
  double fpa_temp() const { return words[HDR_FPA_TEMP] / 100.0; }

  std::string describe() const;
};

}  // namespace uti120

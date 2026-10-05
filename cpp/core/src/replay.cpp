#include "uti120/replay.hpp"

#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <thread>
#include <vector>

namespace uti120 {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto FRAME_PERIOD = std::chrono::milliseconds(40);

class ReplayTransport final : public Transport {
 public:
  explicit ReplayTransport(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw DeviceError("cannot open " + path);
    std::vector<uint8_t> buf(FRAME_BYTES);
    while (in.read(reinterpret_cast<char*>(buf.data()), FRAME_BYTES)) {
      Frame f = Frame::parse(buf);  // rejects anything that is not a camera frame
      (f.shutter_closed() ? closed_ : open_).push_back(buf);
    }
    if (closed_.empty() || open_.empty())
      throw DeviceError(path + " needs frames with the shutter both closed and open");
  }

  void write(uint8_t, const std::vector<uint8_t>& data, unsigned) override {
    if (data == std::vector<uint8_t>{REQUEST_FRAME}) {
      queue_frame();
      return;
    }
    uint8_t func = data.at(0), offset = data.at(1), count = data.at(2);
    if (func == SYS_WRITE || func == SENSOR_WRITE) {
      if (func == SENSOR_WRITE && offset == SREG_SHUTTER) shutter_closed_ = data.at(6) != 0;
      replies_.push_back({func, offset, count});
    } else {
      std::vector<uint8_t> reply{func, uint8_t(4 * count)};
      reply.resize(2 + 4 * count, 0);
      replies_.push_back(reply);
    }
  }

  std::optional<std::vector<uint8_t>> read(uint8_t ep, int max_len, unsigned timeout_ms) override {
    auto& queue = ep == EP_CMD_IN ? replies_ : bulk_;
    if (queue.empty()) {
      // Nothing pending: behave like a USB read that times out.
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
      return std::nullopt;
    }
    std::vector<uint8_t> out = std::move(queue.front());
    queue.pop_front();
    if (int(out.size()) > max_len) {
      queue.emplace_front(out.begin() + max_len, out.end());
      out.resize(max_len);
    }
    return out;
  }

 private:
  void queue_frame() {
    // Pace delivery like the camera's 25 Hz sensor.
    auto due = last_frame_ + FRAME_PERIOD;
    if (Clock::now() < due) std::this_thread::sleep_until(due);
    last_frame_ = Clock::now();

    auto& frames = shutter_closed_ ? closed_ : open_;
    size_t& next = shutter_closed_ ? next_closed_ : next_open_;
    std::vector<uint8_t> frame = frames[next++ % frames.size()];
    // Renumber, so looping never looks like a stale frame, and re-seal the CRC.
    frame[2 * HDR_FRAME_ID] = uint8_t(counter_);
    frame[2 * HDR_FRAME_ID + 1] = uint8_t(counter_ >> 8);
    ++counter_;
    uint32_t crc = crc32(0L, frame.data(), FRAME_BYTES - 4);
    for (int i = 0; i < 4; ++i) frame[FRAME_BYTES - 4 + i] = uint8_t(crc >> (8 * i));
    bulk_.push_back(std::move(frame));
  }

  std::vector<std::vector<uint8_t>> closed_, open_;
  size_t next_closed_ = 0, next_open_ = 0;
  bool shutter_closed_ = false;
  uint16_t counter_ = 1;
  Clock::time_point last_frame_{};
  std::deque<std::vector<uint8_t>> replies_, bulk_;
};

}  // namespace

std::unique_ptr<Transport> open_replay(const std::string& raw_path) {
  return std::make_unique<ReplayTransport>(raw_path);
}

}  // namespace uti120

// Offline tests on frames recorded from a real UTi120Mobile (tests/data, shared
// with the Python implementation).

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

#include "uti120/device.hpp"
#include "uti120/pipeline.hpp"
#include "uti120/palette.hpp"
#include "uti120/process.hpp"

using namespace uti120;

namespace {

int g_failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> r;
  return r;
}
struct Register {
  Register(const char* name, std::function<void()> f) { registry().emplace_back(name, f); }
};

#define TEST(name)                                  \
  void name();                                      \
  Register reg_##name(#name, name);                 \
  void name()

#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                \
    }                                                                              \
  } while (0)

template <class E, class F>
bool throws(F&& f, const char* substring) {
  try {
    f();
  } catch (const E& e) {
    return std::string(e.what()).find(substring) != std::string::npos;
  }
  return false;
}

std::vector<std::vector<uint8_t>> load(const char* name) {
  std::string path = std::string(TEST_DATA_DIR) + "/" + name;
  gzFile gz = gzopen(path.c_str(), "rb");
  if (!gz) throw std::runtime_error("cannot open " + path);
  std::vector<std::vector<uint8_t>> frames;
  for (;;) {
    std::vector<uint8_t> buf(FRAME_BYTES);
    int n = gzread(gz, buf.data(), FRAME_BYTES);
    if (n <= 0) break;
    if (size_t(n) != FRAME_BYTES) throw std::runtime_error("truncated fixture " + path);
    frames.push_back(std::move(buf));
  }
  gzclose(gz);
  return frames;
}

std::vector<Frame> parse_all(const char* name) {
  std::vector<Frame> out;
  for (auto& b : load(name)) out.push_back(Frame::parse(b));
  return out;
}

double stddev(const std::vector<float>& v) {
  double m = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
  double s = 0;
  for (float x : v) s += (x - m) * (x - m);
  return std::sqrt(s / double(v.size()));
}

std::vector<float> mean_of(const std::vector<std::vector<float>>& planes) {
  std::vector<float> out(planes[0].size(), 0.0f);
  for (auto& p : planes)
    for (size_t i = 0; i < p.size(); ++i) out[i] += p[i] / float(planes.size());
  return out;
}

// Replays scripted replies for the command and bulk endpoints.  A command
// reply becomes readable only once a (non-frame-request) command is written.
class FakeUsb : public Transport {
 public:
  std::deque<std::vector<uint8_t>> cmd_replies, pending;
  std::deque<std::optional<std::vector<uint8_t>>> bulk;  // nullopt = timeout
  std::vector<std::vector<uint8_t>> written;

  void write(uint8_t, const std::vector<uint8_t>& data, unsigned) override {
    written.push_back(data);
    if (data != std::vector<uint8_t>{REQUEST_FRAME} && !cmd_replies.empty()) {
      pending.push_back(cmd_replies.front());
      cmd_replies.pop_front();
    }
  }

  std::optional<std::vector<uint8_t>> read(uint8_t ep, int max_len, unsigned) override {
    if (ep == EP_CMD_IN) {
      if (pending.empty()) return std::nullopt;
      auto r = pending.front();
      pending.pop_front();
      return r;
    }
    if (bulk.empty()) return std::nullopt;
    auto r = bulk.front();
    bulk.pop_front();
    if (r && int(r->size()) > max_len) r->resize(max_len);
    return r;
  }
};

FakeUsb* g_fake = nullptr;
Camera fake_camera() {
  auto t = std::make_unique<FakeUsb>();
  g_fake = t.get();
  return Camera(std::move(t));
}

void push_chunks(FakeUsb& usb, const std::vector<uint8_t>& frame) {
  for (size_t i = 0; i < frame.size(); i += CHUNK)
    usb.bulk.emplace_back(std::vector<uint8_t>(frame.begin() + i,
                                               frame.begin() + std::min(frame.size(), i + CHUNK)));
}

std::vector<uint8_t> bytes(std::initializer_list<int> v) {
  return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> load_golden(const std::string& name) {
  std::string path = std::string(TEST_DATA_DIR) + "/golden/" + name + ".gz";
  gzFile gz = gzopen(path.c_str(), "rb");
  if (!gz) throw std::runtime_error("cannot open " + path);
  std::vector<uint8_t> out;
  uint8_t buf[65536];
  for (int n; (n = gzread(gz, buf, sizeof buf)) > 0;) out.insert(out.end(), buf, buf + n);
  gzclose(gz);
  return out;
}

template <class T>
std::vector<T> golden(const std::string& name) {
  auto raw = load_golden(name);
  std::vector<T> out(raw.size() / sizeof(T));
  std::memcpy(out.data(), raw.data(), out.size() * sizeof(T));
  return out;
}

// Largest absolute difference; -1 if the sizes differ.
template <class A, class B>
double max_diff(const std::vector<A>& a, const std::vector<B>& b) {
  if (a.size() != b.size()) return -1;
  double m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(double(a[i]) - double(b[i])));
  return m;
}

// The reference outputs in tests/data/golden are written by the Python
// implementation (tests/make_golden.py); both implementations must agree.
TEST(test_matches_python_palettes) {
  for (const char* name : palette::NAMES) {
    auto lut = palette::lut(name);
    std::vector<uint8_t> flat(&lut[0][0], &lut[0][0] + 256 * 3);
    CHECK(max_diff(flat, golden<uint8_t>(std::string("lut_") + name + ".u8")) == 0);
  }
}

TEST(test_matches_python_processing) {
  auto closed = parse_all("closed16.bin.gz"), opened = parse_all("open4.bin.gz");
  Calibration cal = Calibration::from_frames(closed);
  CHECK(max_diff(cal.dark(), golden<float>("dark.f32")) == 0);
  CHECK(max_diff(cal.bad(), golden<uint8_t>("bad.u8")) == 0);
  AutoGain gain;
  auto lut = palette::lut("ironbow");
  for (size_t k = 0; k < opened.size(); ++k) {
    Plane s = cal.apply(opened[k].pixels());
    CHECK(max_diff(s, golden<float>("signal" + std::to_string(k) + ".f32")) == 0);
    auto rgb = palette::colorize(gain(s), lut);
    CHECK(max_diff(rgb, golden<uint8_t>("ironbow" + std::to_string(k) + ".u8")) == 0);
  }
  Frame f = opened[0];
  uint16_t* px = const_cast<uint16_t*>(f.pixels());
  for (int y = 30; y < 35; ++y)
    for (int x = 50; x < 55; ++x) px[y * WIDTH + x] = ADC_MAX;
  px[0] = 0;
  Plane s = cal.apply(px);
  CHECK(max_diff(s, golden<float>("forced.f32")) == 0);
  CHECK(std::abs(row_stripes(s) - golden<double>("forced_row_stripes.f64")[0]) < 1e-9);
}

// Rotating the rendered image back by hand must give the unrotated rendering,
// and 180 degrees must equal mirror + flip.
TEST(test_renderer_orientation) {
  auto opened = parse_all("open4.bin.gz");
  Calibration cal = Calibration::from_frames(parse_all("closed16.bin.gz"));
  Plane s = cal.apply(opened[0].pixels());
  auto render = [&](View v) { return Renderer(v)(s); };
  RgbImage base = render({});
  CHECK(base.width == WIDTH && base.height == HEIGHT);
  for (int rot : {90, 180, 270}) {
    RgbImage r = render({"ironbow", false, false, rot});
    bool swap = rot != 180;
    CHECK(r.width == (swap ? HEIGHT : WIDTH) && r.height == (swap ? WIDTH : HEIGHT));
    bool same = true;
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x) {
        // Where the clockwise rotation puts sensor pixel (x, y).
        int rx = rot == 90 ? HEIGHT - 1 - y : rot == 180 ? WIDTH - 1 - x : y;
        int ry = rot == 90 ? x : rot == 180 ? HEIGHT - 1 - y : WIDTH - 1 - x;
        for (int c = 0; c < 3; ++c)
          same &= r.rgb[3 * (ry * r.width + rx) + c] == base.rgb[3 * (y * WIDTH + x) + c];
      }
    CHECK(same);
  }
  CHECK(render({"ironbow", true, true, 0}).rgb == render({"ironbow", false, false, 180}).rgb);
  CHECK(throws<std::invalid_argument>([&] { render({"ironbow", false, false, 45}); }, "rotation"));
}

TEST(test_header) {
  auto closed = parse_all("closed16.bin.gz"), opened = parse_all("open4.bin.gz");
  for (auto& f : closed) CHECK(f.shutter_closed());
  for (auto& f : opened) CHECK(!f.shutter_closed());
  for (size_t i = 1; i < closed.size(); ++i) CHECK(closed[i].frame_id() > closed[i - 1].frame_id());
  for (auto* set : {&closed, &opened})
    for (auto& f : *set) CHECK(f.fpa_temp() > 0 && f.fpa_temp() < 60);
}

TEST(test_crc_and_length_are_checked) {
  auto raw = load("open4.bin.gz")[0];
  raw[1000] ^= 1;
  CHECK(throws<FrameError>([&] { Frame::parse(raw); }, "CRC"));
  CHECK(throws<FrameError>([&] { Frame::parse(std::span(raw).first(FRAME_BYTES - 1)); }, "bytes"));
  // A frame with a valid CRC but wrong magic is still rejected.
  raw = load("open4.bin.gz")[0];
  raw[0] = 0;
  uint32_t crc = crc32(0L, raw.data(), FRAME_BYTES - 4);
  for (int i = 0; i < 4; ++i) raw[FRAME_BYTES - 4 + i] = uint8_t(crc >> (8 * i));
  CHECK(throws<FrameError>([&] { Frame::parse(raw); }, "magic"));
}

TEST(test_shutter_calibration_removes_fixed_pattern) {
  auto closed = parse_all("closed16.bin.gz"), opened = parse_all("open4.bin.gz");
  Calibration cal = Calibration::from_frames({closed.begin(), closed.begin() + 8});
  // Raw detector output is dominated by per-pixel offsets...
  std::vector<float> raw(closed[8].pixels(), closed[8].pixels() + PIXELS);
  // ...which the closed-shutter reference removes, leaving temporal noise only.
  double noise = stddev(cal.apply(closed[8].pixels()));
  CHECK(stddev(raw) > 5 * noise);
  // The scene (a running motherboard, warmer than the shutter) is well above that noise.
  std::vector<std::vector<float>> planes;
  for (auto& f : opened) planes.push_back(cal.apply(f.pixels()));
  auto scene = mean_of(planes);
  CHECK(stddev(scene) > 5 * noise);
  CHECK(median(std::vector<double>(scene.begin(), scene.end())) > 0);
}

TEST(test_bad_pixels_are_replaced) {
  auto closed = parse_all("closed16.bin.gz");
  Calibration cal = Calibration::from_frames(closed);
  Frame f = closed[0];
  uint16_t* px = const_cast<uint16_t*>(f.pixels());
  px[40 * WIDTH + 60] = ADC_MAX;
  auto out = cal.apply(px);
  float lo = 1e30f, hi = -1e30f;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
      if (dy || dx) {
        lo = std::min(lo, out[(40 + dy) * WIDTH + 60 + dx]);
        hi = std::max(hi, out[(40 + dy) * WIDTH + 60 + dx]);
      }
  CHECK(lo <= out[40 * WIDTH + 60] && out[40 * WIDTH + 60] <= hi);
  CHECK(cal.bad_count() < 0.01 * PIXELS);  // the closed-shutter frames themselves are clean
}

TEST(test_numpy_semantics) {
  CHECK(median({3, 1, 2}) == 2);
  CHECK(median({4, 1, 3, 2}) == 2.5);
  CHECK(percentile({1, 2, 3, 4, 5}, 50) == 3);
  CHECK(std::abs(percentile({0, 10}, 1) - 0.1) < 1e-12);
}

TEST(test_palette) {
  for (const char* name : palette::NAMES) palette::lut(name);
  auto rgb = palette::colorize({0.0f, 1.0f}, palette::lut("grey"));
  CHECK(rgb == bytes({0, 0, 0, 255, 255, 255}));
  CHECK(throws<std::invalid_argument>([] { palette::lut("nope"); }, "unknown palette"));
}

TEST(test_register_protocol) {
  Camera cam = fake_camera();
  g_fake->cmd_replies = {bytes({0x05, 0x04, 0x07, 0x01, 0x01, 0x03}), bytes({0x0a, 0x03, 0x01})};
  CHECK(cam.read_regs(0x05, 0x03) == std::vector<uint32_t>{0x07010103});
  cam.set_shutter(true);
  CHECK(g_fake->written ==
        (std::vector<std::vector<uint8_t>>{bytes({0x05, 0x03, 0x01}),
                                           bytes({0x0a, 0x03, 0x01, 0, 0, 0, 1})}));
}

TEST(test_late_reply_is_not_taken_for_next_one) {
  Camera cam = fake_camera();
  g_fake->cmd_replies = {bytes({0x0a, 0x03, 0x01})};
  g_fake->pending.push_back(bytes({0x0a, 0x03, 0x01}));  // late reply to an earlier request
  cam.set_shutter(false);
  CHECK(g_fake->pending.empty());
}

TEST(test_register_errors) {
  Camera a = fake_camera();
  CHECK(throws<DeviceError>([&] { a.read_regs(0x05, 0x07); }, "no reply"));
  Camera b = fake_camera();
  g_fake->cmd_replies = {bytes({0x0a, 0x04, 0x01})};
  CHECK(throws<DeviceError>([&] { b.set_shutter(false); }, "bad reply"));
}

TEST(test_grab_drops_stale_frames) {
  auto frames = load("open4.bin.gz");
  auto opened = parse_all("open4.bin.gz");
  Camera cam = fake_camera();
  g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frames[1]);
  g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frames[0]);
  g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frames[1]);
  CHECK(cam.grab().frame_id() == opened[1].frame_id());
  // The older frame is rejected, the request repeated, and the same id is not accepted twice.
  CHECK(throws<DeviceError>([&] { cam.grab(2); }, "no valid frame"));
  CHECK(g_fake->written.size() == 3);
}

TEST(test_start_discards_frame_left_from_previous_session) {
  auto frames = load("open4.bin.gz");
  auto opened = parse_all("open4.bin.gz");
  Camera cam = fake_camera();
  g_fake->cmd_replies = {bytes({0x04, 0xf0, 0x01})};
  g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frames[3]);  // left over, newer id than the live one
  g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frames[0]);
  cam.start();
  CHECK(g_fake->written[0] == bytes({0x04, 0xf0, 0x01, 0, 0, 0, 2}));
  CHECK(cam.grab().frame_id() == opened[0].frame_id());
}

TEST(test_grab_repeats_ignored_requests) {
  auto frame = load("open4.bin.gz")[0];
  Camera cam = fake_camera();
  // drain sees nothing, first request ignored (timeout), drain, second answered
  for (int i = 0; i < 3; ++i) g_fake->bulk.emplace_back(std::nullopt);
  push_chunks(*g_fake, frame);
  Frame f = cam.grab();
  CHECK(std::equal(frame.begin(), frame.end(), reinterpret_cast<const uint8_t*>(f.words.data())));
  CHECK(g_fake->written.size() == 2);
}

}  // namespace

int main() {
  for (auto& [name, f] : registry()) {
    int before = g_failures;
    try {
      f();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  unexpected exception: %s\n", e.what());
      ++g_failures;
    }
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
  }
  std::printf("%zu tests, %d failed checks\n", registry().size(), g_failures);
  return g_failures ? 1 : 0;
}

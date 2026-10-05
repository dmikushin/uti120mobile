// Translation of src/uti120/vendor_y16.py, vendor_temp.py and radiometry.py.
// The arithmetic deliberately follows the vendor library: float (not double)
// wherever it uses float32, C integer division, int16 wrap-around.  The core is
// compiled with -ffp-contract=off so that no multiply-add is fused (that would
// change float results on arm64).

#include "uti120/radiometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "uti120/log.hpp"

namespace uti120 {

namespace detail {
// Generated at build time from src/uti120/data/emiss_curve.bin (cmake/embed_emiss.cmake):
// nEmissCurve, the vendor library's static table of temperature * 10 for each
// of 0x4000 radiance levels, as little-endian int16.
extern const unsigned char kEmissCurveBytes[];
extern const std::size_t kEmissCurveSize;
}  // namespace detail

static constexpr const char* LOG = "uti120.radiometry";

namespace {

const std::vector<int32_t>& emiss_curve() {
  static const std::vector<int32_t> table = [] {
    if (detail::kEmissCurveSize != 2 * 0x4000) throw std::logic_error("emissivity table size");
    std::vector<int32_t> t(0x4000);
    for (int i = 0; i < 0x4000; ++i)
      t[i] = int16_t(uint16_t(detail::kEmissCurveBytes[2 * i]) |
                     uint16_t(detail::kEmissCurveBytes[2 * i + 1]) << 8);
    return t;
  }();
  return table;
}

template <class T>
T read_le(const std::vector<uint8_t>& d, size_t off) {
  T v;
  std::memcpy(&v, d.data() + off, sizeof v);
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Calibration package

namespace vendor {

Package Package::parse(const std::vector<uint8_t>& d) {
  constexpr size_t HEADER = 0xd8, DATA = 0xf6;
  if (d.size() < DATA) throw std::invalid_argument("calibration package too short");
  Package p;
  p.n_focus = d[0x41];
  p.n_dist = d[0x4b];
  p.t_min = read_le<int16_t>(d, 0x42);
  p.t_max = read_le<int16_t>(d, 0x44);
  int width = read_le<uint16_t>(d, 0x4c), height = read_le<uint16_t>(d, 0x4e);
  p.curve_len = read_le<uint16_t>(d, 0x50);
  size_t focus_bytes = read_le<uint16_t>(d, 0x52);
  int64_t curve_bytes = read_le<int32_t>(d, 0x54);
  int64_t k_bytes = read_le<int32_t>(d, 0x58);
  if (width != WIDTH || height != HEIGHT || p.n_focus < 1 || p.n_dist < 2 || p.curve_len < 2 ||
      focus_bytes != size_t(2 * p.n_focus) ||
      curve_bytes != int64_t(2) * p.n_focus * p.n_dist * p.curve_len ||
      k_bytes != int64_t(2) * p.n_focus * PIXELS ||
      d.size() < DATA + focus_bytes + size_t(curve_bytes) + size_t(k_bytes) ||
      0x74 + 2 * size_t(p.n_dist) > HEADER)
    throw std::invalid_argument("unexpected calibration package geometry");

  size_t off = DATA;
  for (int i = 0; i < p.n_focus; ++i) {
    int16_t f = read_le<int16_t>(d, off + 2 * i);
    p.focus_raw.push_back(f);
    p.focus.push_back(float(f) / 100.0f);
  }
  off += focus_bytes;
  size_t n_curve = size_t(curve_bytes) / 2;
  p.curves.resize(n_curve);
  for (size_t i = 0; i < n_curve; ++i) p.curves[i] = read_le<uint16_t>(d, off + 2 * i);
  off += size_t(curve_bytes);
  p.k.resize(size_t(k_bytes) / 2);
  std::memcpy(p.k.data(), d.data() + off, size_t(k_bytes));
  for (int i = 0; i < p.n_dist; ++i)
    p.distances.push_back(float(read_le<uint16_t>(d, 0x74 + 2 * i)) / 10.0f);
  for (size_t i = 0x92; i < 0xa4 && d[i]; ++i) p.serial += char(d[i]);

  // GetSingleCurveTemperature looks for the first point after index 0 above a
  // value with a linear scan (the curves are not monotonic at their ends, so a
  // binary search on the curve itself would differ).  The first point above v
  // is also the first index where the running maximum exceeds v, and the
  // running maximum is sorted, so it can be binary searched instead.
  p.curve_max = p.curves;
  for (int c = 0; c < p.n_focus * p.n_dist; ++c) {
    int32_t* m = p.curve_max.data() + size_t(c) * p.curve_len;
    for (int j = 2; j < p.curve_len; ++j) m[j] = std::max(m[j], m[j - 1]);
  }
  return p;
}

// ---------------------------------------------------------------------------
// Raw frame -> Y16

namespace {

constexpr int GAUSS3[3][3] = {{46, 343, 46}, {343, 2536, 343}, {46, 343, 46}};

// T[d] = round(4096 * exp(-d^2 / (2 * 25^2))) for |differences| up to 511.
const std::array<int64_t, 512>& range_weights() {
  static const std::array<int64_t, 512> t = [] {
    std::array<int64_t, 512> w{};
    for (int d = 0; d < 512; ++d)
      w[d] = int64_t(std::floor(4096.0 * std::exp(-double(d) * d / (2.0 * 25 * 25)) + 0.5));
    return w;
  }();
  return t;
}

using Image16 = std::vector<int16_t>;  // HEIGHT x WIDTH

inline int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

// CInfraredCore::ReplaceBadPoint for pixels with K bit 15 set: descending sort
// of the valid 8-neighbours; their median, or for an even count
// a[n/2-1]/2 + a[n/2]/2 with C division.  Replaced pixels are never read
// (bad neighbours are skipped), so the in-place scan needs no copy.
void replace_bad_points(Image16& img, const uint16_t* k) {
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      if (!(k[y * WIDTH + x] & 0x8000)) continue;
      int64_t vals[8];
      int n = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (!dy && !dx) continue;
          int yy = y + dy, xx = x + dx;
          if (yy < 0 || yy >= HEIGHT || xx < 0 || xx >= WIDTH || (k[yy * WIDTH + xx] & 0x8000)) continue;
          vals[n++] = img[yy * WIDTH + xx];
        }
      if (!n) continue;
      for (int i = 1; i < n; ++i)  // descending insertion sort of at most 8 values
        for (int j = i; j > 0 && vals[j - 1] < vals[j]; --j) std::swap(vals[j - 1], vals[j]);
      int64_t v = n % 2 ? vals[n / 2] : vals[n / 2 - 1] / 2 + vals[n / 2] / 2;
      img[y * WIDTH + x] = int16_t(v);
    }
}

// FixedPoint_GrayFilter_16bit_RSN: range-weighted smoothing over a wx x wy
// window with edge replication.  F = sum(T*v) / sum(T) (the centre if the
// weights vanish), W = sum(T) / count.
void rsn_filter(const Image16& img, int wx, int wy, Image16& f, std::vector<uint16_t>& w) {
  const auto& table = range_weights();
  int hx = wx / 2, hy = wy / 2;
  int64_t count = int64_t(wx) * wy;
  f.resize(PIXELS);
  w.resize(PIXELS);
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      int64_t c = img[y * WIDTH + x], sw = 0, swv = 0;
      for (int dy = 0; dy < wy; ++dy)
        for (int dx = 0; dx < wx; ++dx) {
          int64_t v = img[clampi(y + dy - hy, 0, HEIGHT - 1) * WIDTH + clampi(x + dx - hx, 0, WIDTH - 1)];
          int64_t wgt = table[std::min<int64_t>(std::llabs(c - v), 511)];
          sw += wgt;
          swv += wgt * v;
        }
      f[y * WIDTH + x] = int16_t(sw >= 1 ? swv / sw : c);
      w[y * WIDTH + x] = uint16_t((uint64_t(sw) & 0xFFFFFFFFu) / uint64_t(count));
    }
}

// RemoveVerStripe (vertical: per-column offsets, 9 x 1 window) and
// RemoveHorStripe (per-row offsets, 1 x 9 window): the mean of
// clamp(in - F) over the pixels whose weight reaches the threshold, clamped,
// is subtracted from the column / row.
void remove_stripes(Image16& img, bool vertical, int win, int thresh, int clamp) {
  Image16 f;
  std::vector<uint16_t> w;
  if (vertical)
    rsn_filter(img, win, 1, f, w);
  else
    rsn_filter(img, 1, win, f, w);
  int lines = vertical ? WIDTH : HEIGHT, along = vertical ? HEIGHT : WIDTH;
  std::vector<int16_t> corr(lines);
  for (int l = 0; l < lines; ++l) {
    int64_t s = 0, n = 0;
    for (int a = 0; a < along; ++a) {
      int i = vertical ? a * WIDTH + l : l * WIDTH + a;
      if (w[i] < thresh) continue;
      s += std::clamp<int64_t>(int64_t(img[i]) - f[i], -clamp, clamp);
      ++n;
    }
    int64_t m = n >= 1 ? s / n : 0;
    corr[l] = int16_t(std::clamp<int64_t>(m, -clamp, clamp));
  }
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      int i = y * WIDTH + x;
      img[i] = int16_t(int64_t(img[i]) - corr[vertical ? x : y]);
    }
}

// GaussianFilter_16bit: 3x3 kernel, edge replication, (uint32)sum >> 12.
Image16 gauss3(const Image16& img) {
  Image16 out(PIXELS);
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      int64_t s = 0;
      for (int dy = 0; dy < 3; ++dy)
        for (int dx = 0; dx < 3; ++dx)
          s += GAUSS3[dy][dx] *
               int64_t(img[clampi(y + dy - 1, 0, HEIGHT - 1) * WIDTH + clampi(x + dx - 1, 0, WIDTH - 1)]);
      out[y * WIDTH + x] = int16_t(uint32_t(uint64_t(s) & 0xFFFFFFFFu) >> 12);
    }
  return out;
}

}  // namespace

void Y16Model::advance(const Frame& frame, const Package& package) {
  // After the image: a shutter frame becomes the new B (guideCoreUpdateB is
  // called after ConvertXToImage), and the K gear follows the frame's FPA
  // temperature (updateK): below the first knot 0, above the last n, else
  // i + 1 for the first i with focus[i] <= fpa <= focus[i+1].
  if (frame.shutter_closed()) std::memcpy(b_.data(), frame.pixels(), PIXELS * sizeof(uint16_t));
  const auto& f = package.focus_raw;
  int n = int(f.size());
  int fpa = int16_t(frame.words[HDR_FPA_TEMP]);
  if (fpa < f[0]) {
    gear_ = 0;
  } else if (fpa > f[n - 1]) {
    gear_ = n;
  } else {
    gear_ = 1;
    for (int i = 0; i + 1 < n; ++i)
      if (f[i] <= fpa && fpa <= f[i + 1]) {
        gear_ = i + 1;
        break;
      }
  }
}

void Y16Model::skip(const Frame& frame, const Package& package) { advance(frame, package); }

std::vector<int16_t> Y16Model::process(const Frame& frame, const Package& package) {
  // The vendor selects gear n (one past its K table) when the FPA is above the
  // last knot (above ~65 C; its own core then reads out of bounds and crashes).
  // The last gear is used here instead.
  const uint16_t* k = package.k.data() + size_t(std::min(gear_, package.n_focus - 1)) * PIXELS;
  const uint16_t* x = frame.pixels();
  // NUC: (X - B) * gain / 8192, C division, wrapped to int16.
  Image16 img(PIXELS);
  for (int i = 0; i < PIXELS; ++i)
    img[i] = int16_t((int64_t(x[i]) - int64_t(b_[i])) * int64_t(k[i] & 0x7FFF) / 8192);
  replace_bad_points(img, k);  // applied twice by the vendor; idempotent
  // The temporal filter is switched off (guideCoreConvertXToImage: put_tff_switch(false)).
  remove_stripes(img, true, 9, 3200, 25);
  remove_stripes(img, false, 9, 3500, 20);
  Image16 out = gauss3(img);
  advance(frame, package);
  return out;
}

// ---------------------------------------------------------------------------
// Y16 -> temperature

void MeasureState::update(const int16_t* w) {
  startup_shutter = float(w[HDR_SHUTTER_TEMP_INIT]) / 100.0f;
  shutter = float(w[HDR_SHUTTER_TEMP]) / 100.0f;
  lens = float(w[HDR_TUBE_TEMP]) / 100.0f;
  fpa = float(w[HDR_FPA_TEMP]) / 100.0f;
  fpa_raw = w[HDR_FPA_TEMP];
  shutter_flag = w[HDR_SHUTTER_CLOSED];
  if (last_shutter_flag == 1 && shutter_flag == 0) {
    lastlast_shutter = last_shutter;
    last_shutter = shutter;
    lastlast_lens = last_lens;
    last_lens = lens;
    last_fpa = fpa;
  }
  last_shutter_flag = shutter_flag;
}

void MeasureState::update(const Frame& frame) {
  int16_t w[16];
  for (int i = 0; i < 16; ++i) w[i] = int16_t(frame.words[i]);
  update(w);
}

namespace {

// initMeasureParam constants.
constexpr float LENS_K_HIGH = -200.0f;  // MEASURE_PARAM+0x74 (GetHighTemperature)
constexpr float LENS_K_LOW = -300.0f;   // MEASURE_PARAM+0x78 (GetLowTemperature)
constexpr float EMISS_MAX = 0.98f;      // DAT_00142cc8
constexpr double DRIFT_MIN = 0.1;       // DAT_00142d30

// LensDriftCorrect: Y16 drift from the lens warming since the last shutter.
float lens_drift(const MeasureState& s, bool high) {
  if (double(s.last_fpa) >= DRIFT_MIN && double(s.last_lens) >= DRIFT_MIN)
    return (s.lens - s.last_lens) * (high ? LENS_K_HIGH : LENS_K_LOW);
  return 0.0f;
}

// Index part of GetSingleCurveTemperature: the first point after index 0
// above v; 0 below the first point, curve_len above the last, 0 if none.
int search(const int32_t* curve, const int32_t* running_max, int n, int v) {
  if (v < curve[0]) return 0;
  if (curve[n - 1] < v) return n;
  const int32_t* it = std::upper_bound(running_max + 1, running_max + n, v);
  return it == running_max + n ? 0 : int(it - running_max);
}

// GetSingleCurveTemperature for one curve.
float single_curve(int y16, const Package& p, int f, int d, const MeasureState& s, bool high) {
  const int32_t* curve = p.curve(f, d);
  float corr = lens_drift(s, high);
  int y = int16_t(int(float(int16_t(y16)) - corr));         // (short)(int)((float)y16 - corr)
  int base = int(s.shutter * 10.0f - 10.0f * float(p.t_min));
  float offset = 0 < base && base < p.curve_len ? float(curve[base]) : 0.0f;
  int v = int(float(y) * 1.0f + offset);                     // factor MEASURE_PARAM+0x4c*0x5c = 1
  int j = search(curve, p.curve_running_max(f, d), p.curve_len, v);
  float frac = float(double(j) / 10.0);
  return float(0.0 + double(float(p.t_min)) + double(frac));  // offsets +0x50, +0x60 = 0
}

// GetY16FromT: radiance level of temperature * 10 (binary search in nEmissCurve).
int y16_from_t(int t10) {
  const auto& c = emiss_curve();
  if (!(c[0] < t10 && t10 < c[0x3fff])) return t10 >= c[0x3fff] ? 0x3fff : 0;
  int lo = 0, hi = 0x3fff;
  for (;;) {
    int mid;
    for (;;) {
      mid = (hi + lo) >> 1;
      if (c[mid] < t10) break;
      if (c[mid] <= t10) return mid;
      hi = mid - 1;
      if (hi < lo) return mid;
    }
    lo = mid + 1;
    if (lo > hi) return mid;
  }
}

// EmissCor(short T*10, short Y16 of the reflected temperature, int emissivity*100).
int emiss_cor(int t10, int y16_reflect, int e100) {
  const auto& c = emiss_curve();
  if (e100 < 1) return c[0x3fff] & 0xFFFF;
  if (e100 > 99) return t10;
  int t = int16_t(t10);
  int u = 0x3fff;
  if (c[0x3fff] > t && c[0] < t) {
    int lo = 0, hi = 0x3fff;
    for (;;) {
      u = (hi + lo) >> 1;
      if (c[u] < t) {
        lo = u + 1;
        if (lo > hi) break;
        continue;
      }
      if (c[u] <= t) break;
      hi = u - 1;
      if (hi < lo) break;
    }
  } else if (!(c[0] < t)) {
    u = 0;
  }
  if (e100 < 99) u = (u * 100 - y16_reflect * (100 - e100)) / e100;  // C division
  u = std::clamp(u, 0, 0x3fff);
  return c[u] & 0xFFFF;
}

}  // namespace

float temperature(int y16, const Package& p, const TemperatureSettings& st, const MeasureState& s,
                  bool high) {
  // getCurve: the FPA knots bracketing the camera's temperature.
  const auto& f = p.focus_raw;
  int n = p.n_focus, fpa = s.fpa_raw;
  int idx, lo, up;  // -1: no curve on that side
  if (fpa < f[0]) {
    idx = 0, lo = -1, up = 0;
  } else if (f[n - 1] < fpa) {
    idx = n, lo = n - 1, up = -1;
  } else {
    idx = 1, lo = 0, up = 0;
    for (int i = 0; i < n; ++i) {
      int upper = i + 1 < n ? i + 1 : i;
      if (f[i] <= fpa && fpa <= f[upper]) {
        idx = i + 1, lo = i, up = upper;
        break;
      }
    }
  }
  float t_lo0 = 0, t_lo1 = 0, t_up0 = 0, t_up1 = 0;
  if (lo >= 0) t_lo0 = single_curve(y16, p, lo, 0, s, high), t_lo1 = single_curve(y16, p, lo, 1, s, high);
  if (up >= 0) t_up0 = single_curve(y16, p, up, 0, s, high), t_up1 = single_curve(y16, p, up, 1, s, high);

  // FPA weights (GetLowTemperature).
  float w_up, w_lo;
  if (idx == 0) {
    w_up = 1.0f, w_lo = 0.0f;
  } else if (idx == n) {
    w_up = 0.0f, w_lo = 1.0f;
  } else {
    float a = p.focus[idx] - s.fpa;
    float b = s.fpa - p.focus[idx - 1];
    float sum = b + a;
    w_up = 1.0f - a / sum;
    w_lo = 1.0f - b / sum;
  }

  // Distance weights between the first two calibration distances (in double,
  // with a float sum, like the vendor).
  float dist = st.distance, d0 = p.distances[0], d1 = p.distances[1];
  float w_d0, w_d1;
  if (dist <= d0) {
    w_d0 = 1.0f, w_d1 = 0.0f;
  } else if (dist < d1) {
    double a = double(dist - d0), b = double(d1 - dist);
    double sum = double(float(b + a));
    w_d0 = float(1.0 - a / sum);
    w_d1 = float(1.0 - b / sum);
  } else {
    w_d0 = 0.0f, w_d1 = 1.0f;
  }

  float t = (w_up * t_up0 + w_lo * t_lo0) * w_d0 + (w_up * t_up1 + w_lo * t_lo1) * w_d1;

  // Emissivity (EmissCor through the radiance table).
  if (st.emissivity <= EMISS_MAX) {
    int yr = y16_from_t(int(st.reflected * 10.0f));
    int t10 = int16_t(int(t * 10.0f));
    t = float(int16_t(emiss_cor(t10, yr, int(st.emissivity * 100.0f)))) * 0.1f;
  }
  return t;
}

}  // namespace vendor

// ---------------------------------------------------------------------------
// Java-side correction, calibration I/O, Radiometer

float band_correction(float t, const std::array<int32_t, 8>& c, bool high_range) {
  auto coef = [&](int i) { return float(c[i]) / 10000.0f; };
  float k = 1.0f, b = 0.0f;
  if (t <= 0) {
    k = coef(0), b = coef(1);
  } else if (t <= 80) {
    k = coef(2), b = coef(3);
  } else if (t <= 150) {
    k = coef(4), b = coef(5);
  }
  if (high_range && t > 100) k = coef(6), b = coef(7);
  return k > 0 ? k * t + b : t;
}

namespace {

void write_file(const std::filesystem::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), std::streamsize(data.size()));
  if (!out) throw std::runtime_error("cannot write " + path.string());
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

void CameraCalibration::save(const std::string& dir) const {
  std::filesystem::create_directories(dir);
  write_file(std::filesystem::path(dir) / "low.bin", std::string(low.begin(), low.end()));
  write_file(std::filesystem::path(dir) / "high.bin", std::string(high.begin(), high.end()));
  std::string json = "[";
  for (size_t i = 0; i < coefficients.size(); ++i)
    json += (i ? ", " : "") + std::to_string(coefficients[i]);
  write_file(std::filesystem::path(dir) / "coefficients.json", json + "]");
}

CameraCalibration CameraCalibration::load(const std::string& dir) {
  CameraCalibration c;
  std::string low = read_file(std::filesystem::path(dir) / "low.bin");
  std::string high = read_file(std::filesystem::path(dir) / "high.bin");
  c.low.assign(low.begin(), low.end());
  c.high.assign(high.begin(), high.end());
  // A JSON array of eight integers, as written by save() and by the Python implementation.
  std::string json = read_file(std::filesystem::path(dir) / "coefficients.json");
  for (char& ch : json)
    if (ch == '[' || ch == ']' || ch == ',') ch = ' ';
  std::istringstream in(json);
  size_t n = 0;
  for (long long v; in >> v; ++n) {
    if (n >= c.coefficients.size()) break;
    c.coefficients[n] = int32_t(v);
  }
  if (n != c.coefficients.size() || !in.eof())
    throw std::runtime_error("bad coefficients.json in " + dir);
  return c;
}

CameraCalibration read_camera_calibration(Camera& cam) {
  CameraCalibration c;
  // Registers 49..56, one by one like the vendor (InitThread.initCalibrationInfo).
  for (int i = 0; i < 8; ++i) c.coefficients[i] = int32_t(cam.read_regs(SYS_READ, uint8_t(49 + i))[0]);
  c.low = cam.read_calibration(CalibrationPackage::Low);
  c.high = cam.read_calibration(CalibrationPackage::High);
  for (auto* pkg : {&c.low, &c.high}) {
    static const char magic[] = "TI_CAL_METHOD";
    if (pkg->size() < 0x40 ||
        std::search(pkg->begin(), pkg->begin() + 0x40, magic, magic + sizeof magic - 1) == pkg->begin() + 0x40)
      throw DeviceError("calibration package has an unknown format");
    vendor::Package::parse(*pkg);  // validate before it is cached
  }
  return c;
}

Radiometer::Radiometer(const CameraCalibration& c, const RadiometrySettings& settings)
    : coefficients_(c.coefficients),
      low_(vendor::Package::parse(c.low)),
      high_(vendor::Package::parse(c.high)),
      settings_(settings) {
  LOG_INFO(LOG, "calibration of sensor %s: %d FPA knots, curves %d..%d C", low_.serial.c_str(),
           low_.n_focus, low_.t_min, low_.t_max);
}

std::optional<Plane> Radiometer::feed(const Frame& frame) {
  const vendor::Package& pkg = package();
  bool closed = frame.shutter_closed();
  if (closed || !have_reference_) {
    y16_.skip(frame, pkg);
    state_.update(frame);
    if (closed) have_reference_ = true;
    return std::nullopt;
  }
  std::vector<int16_t> y16 = y16_.process(frame, pkg);
  state_.update(frame);

  // Within a frame the temperature depends on the Y16 value only: compute it
  // once per distinct value.
  auto [mn, mx] = std::minmax_element(y16.begin(), y16.end());
  int base = *mn, span = *mx - *mn + 1;
  std::vector<uint8_t> present(span, 0);
  for (int16_t v : y16) present[v - base] = 1;
  std::vector<float> lut(span);
  vendor::TemperatureSettings ts{settings_.emissivity, settings_.reflected, settings_.distance};
  for (int i = 0; i < span; ++i)
    if (present[i])
      lut[i] = band_correction(vendor::temperature(base + i, pkg, ts, state_, settings_.high_range),
                               coefficients_, settings_.high_range);
  Plane t(PIXELS);
  for (int i = 0; i < PIXELS; ++i) t[i] = lut[y16[i] - base];
  return t;
}

}  // namespace uti120

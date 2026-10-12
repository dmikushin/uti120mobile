// Temperatures in degrees Celsius, computed the way the vendor app computes them.
//
// The camera stores its calibration in flash: two packages (low and high
// measuring range) holding per-pixel gains and counts-to-temperature curves,
// and eight correction coefficients in system registers 49..56.  The vendor app
// (Thermal Mobile 3.1.2) reads them once, then for every frame
//
//   1. turns the raw frame into a Y16 image (gain/offset correction against the
//      last shutter frame, bad pixels, stripe removal, smoothing),
//   2. maps each Y16 value to a temperature with the curves, corrected for the
//      camera's own temperatures from the frame header,
//   3. applies a linear correction k*t + b chosen by temperature band
//      (MainActivity.genCalibrationValue).
//
// Steps 1 and 2 reproduce the vendor's native library libguide_sdk_unitrend.so
// bit for bit (float32 arithmetic and C integer division included; checked
// against it on recorded frames, see tests); step 3 is its Java code.  This is
// a translation of the Python reference src/uti120/vendor_y16.py,
// vendor_temp.py and radiometry.py, which document the decompiled functions
// each step comes from.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "uti120/device.hpp"
#include "uti120/frame.hpp"
#include "uti120/process.hpp"

namespace uti120 {

// Everything the camera stores for temperature measurement.
struct CameraCalibration {
  std::vector<uint8_t> low;   // calibration package, low range (flash 0x132000)
  std::vector<uint8_t> high;  // calibration package, high range (flash 0x100000)
  std::array<int32_t, 8> coefficients{};  // registers 49..56: k, b of four bands, x10000

  // Files low.bin, high.bin, coefficients.json in `dir` (the same layout as the
  // Python implementation, so both share a cache).  Throw std::runtime_error.
  void save(const std::string& dir) const;
  static CameraCalibration load(const std::string& dir);

  // Throws DeviceError unless both packages parse and belong to `sensor` (each
  // package carries the serial number of its camera); WrongCamera if they
  // belong to another one.
  void validate(const std::string& sensor) const;
};

struct WrongCamera : DeviceError {
  using DeviceError::DeviceError;
};

// Reads the calibration from the camera and leaves it idle.
//
// After a flash upload the firmware stops delivering frames until it is
// rebooted (measured: neither waiting, re-selecting the run status nor clearing
// the bulk endpoint helps; a reboot does).  Callers must therefore reboot the
// camera (write_reg(SYS_WRITE, REG_REBOOT, 1)), wait for it to enumerate again
// and open it anew before streaming; cache the result so this happens once per
// camera.
CameraCalibration read_camera_calibration(Camera& cam);

struct RadiometrySettings {
  float emissivity = 0.95f;  // vendor default (SRateBean)
  float reflected = 23.0f;   // degrees C; the vendor core's default
  float distance = 0.6f;     // metres; vendor default (ConfigBean.refDistant)
  bool high_range = false;   // measuring range: low (default) or high
};

// MainActivity.genCalibrationValue, float32 like the Java code.
float band_correction(float t, const std::array<int32_t, 8>& coefficients, bool high_range);

namespace vendor {

// One calibration package (guideCoreParsePackage), little-endian:
//   0x000..0x0d8  header
//       0x41 u8   n_focus: number of FPA-temperature knots
//       0x42 i16  T_min of the curves (degC); 0x44 i16 T_max
//       0x4b u8   n_dist: curves per knot (one per calibration distance)
//       0x4c u16  width (120); 0x4e u16 height (90)
//       0x50 u16  curve_len: points per curve, 0.1 degC apart from T_min
//       0x52 u16  focus bytes (2 n_focus); 0x54 i32 curve bytes; 0x58 i32 K bytes
//       0x74 u16[n_dist] calibration distances * 10 (m)
//       0x92      sensor serial (ASCII)
//   0x0f6  focus table i16[n_focus] (degC * 100), then the curves
//          u16[n_focus][n_dist][curve_len] (absolute Y16 per temperature), then
//          the K table: n_focus "gears" of u16[90][120], bit 15 = bad pixel,
//          bits 0..14 = per-pixel gain in 1/8192.
// Throws std::invalid_argument for anything else.
struct Package {
  int n_focus = 0, n_dist = 0, curve_len = 0;
  int16_t t_min = 0, t_max = 0;
  std::vector<int16_t> focus_raw;  // degC * 100
  std::vector<float> focus;        // degC
  std::vector<float> distances;    // m
  std::vector<int32_t> curves;     // [n_focus][n_dist][curve_len]
  std::vector<int32_t> curve_max;  // running maximum of each curve from index 1 (search)
  std::vector<uint16_t> k;         // [n_focus][PIXELS]
  std::string serial;

  static Package parse(const std::vector<uint8_t>& data);
  const int32_t* curve(int f, int d) const { return curves.data() + (f * n_dist + d) * curve_len; }
  const int32_t* curve_running_max(int f, int d) const {
    return curve_max.data() + (f * n_dist + d) * curve_len;
  }
};

// Raw frame -> Y16 (CInfraredCore::InfraredImageProcess with the switches the
// vendor app uses).  State across frames: B, the last shutter frame, and the K
// gear chosen from the previous frame's FPA temperature.
class Y16Model {
 public:
  // Y16, 90 x 120 in sensor orientation (the vendor rotates it for display).
  std::vector<int16_t> process(const Frame& frame, const Package& package);
  // The state update of process() without computing the image.
  void skip(const Frame& frame, const Package& package);
  // Measuring range switched (guideCoreSetMeasureMode): gear 0 as after init.
  void reset_gear() { gear_ = 0; }

 private:
  void advance(const Frame& frame, const Package& package);

  std::vector<uint16_t> b_ = std::vector<uint16_t>(PIXELS, 0);
  int gear_ = 0;
};

// MEASURE_PARAM fields fed by updateMeasureParam from every frame header.
struct MeasureState {
  float startup_shutter = 0, shutter = 0, lens = 0, fpa = 0;
  int fpa_raw = 0;
  int shutter_flag = 0, last_shutter_flag = 0;
  float last_shutter = 0, lastlast_shutter = 0, last_lens = 0, lastlast_lens = 0, last_fpa = 0;

  void update(const Frame& frame);
  void update(const int16_t* header_words);  // words 0..15 of the frame
};

struct TemperatureSettings {
  float emissivity = 0.95f;
  float reflected = 23.0f;
  float distance = 0.8f;
};

// guideCoreMeasureTempByY16 (getCurve + GetLowTemperature / GetHighTemperature).
float temperature(int y16, const Package& package, const TemperatureSettings& settings,
                  const MeasureState& state, bool high_range);

}  // namespace vendor

// Feed every frame in order (shutter frames included); open frames yield
// temperatures.
class Radiometer {
 public:
  explicit Radiometer(const CameraCalibration& calibration, const RadiometrySettings& settings = {});

  // Temperatures, 90 x 120 in sensor orientation (like Image::signal); nullopt
  // for shutter frames and until the first shutter frame has been seen (the
  // vendor app does not measure before its first shutter/NUC either).
  std::optional<Plane> feed(const Frame& frame);

  // Takes effect from the next frame.  A range change behaves like the vendor's
  // SetMeasureMode: B and the camera temperature history are kept, the K gear
  // restarts at 0 (measured against the vendor library).
  void set_settings(const RadiometrySettings& settings);
  const RadiometrySettings& settings() const { return settings_; }

 private:
  const vendor::Package& package() const { return settings_.high_range ? high_ : low_; }

  std::array<int32_t, 8> coefficients_;
  vendor::Package low_, high_;
  RadiometrySettings settings_;
  vendor::Y16Model y16_;
  vendor::MeasureState state_;
  bool have_reference_ = false;
};

}  // namespace uti120

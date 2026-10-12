// Calibrated frame stream: start the camera, calibrate on the shutter, yield images.
#pragma once

#include <cstdio>
#include <optional>

#include "uti120/device.hpp"
#include "uti120/process.hpp"
#include "uti120/radiometry.hpp"

namespace uti120 {

// The vendor app ignores frames for 3 s after a NUC (checkNuc in DataInterface_ToGuide_Imp).
constexpr double NUC_SETTLE_S = 3.0;
// Frames still in flight with the previous shutter state after it reports the new one.
constexpr int SHUTTER_SETTLE_FRAMES = 2;

struct Image {
  Frame frame;
  Plane signal;  // offset-corrected counts
  // Degrees C per pixel, same layout as signal; only with radiometry enabled,
  // and not before the radiometer has seen its first shutter frame.
  std::optional<Plane> temperature;
};

class Stream {
 public:
  // raw_sink, if given, receives every raw 25600-byte frame.
  Stream(Camera& cam, int dark_frames = 16, double recalibrate_s = 0.0,
         std::FILE* raw_sink = nullptr);

  // Start streaming, run the on-camera NUC and calibrate on the shutter.
  //
  // The NUC is not optional: the offsets the camera keeps from its last NUC go
  // stale, and then most pixels clip at 0 or 0x3FFF (observed: 8600 of 10800)
  // until a new NUC is done.  On failure the camera is returned to idle.
  void start();
  void calibrate();
  Image read();
  void stop();

  const Calibration& calibration() const { return *calibration_; }

  // From now on every grabbed frame (shutter frames included, in order) is fed
  // to a radiometer, and read() returns temperatures.  Not thread-safe: call it
  // from the thread that reads, or before start().
  void enable_radiometry(const CameraCalibration& calibration, const RadiometrySettings& settings);
  // Changes the settings of an enabled radiometer (same threading rule).
  void set_radiometry(const RadiometrySettings& settings);
  bool radiometry_enabled() const { return radiometer_.has_value(); }

 private:
  Frame grab();
  void wait_shutter(bool closed);

  Camera& cam_;
  int dark_frames_;
  double recalibrate_s_;
  std::FILE* raw_sink_;
  std::optional<Calibration> calibration_;
  double calibrated_at_ = 0.0;
  std::optional<Radiometer> radiometer_;
  std::optional<Plane> last_temperature_;  // of the most recently grabbed frame
};

double monotonic_s();

}  // namespace uti120

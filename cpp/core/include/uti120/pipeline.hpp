// The backend facade shared by the frontends (desktop CLI, Android app):
// camera start-up and calibration, a capture thread that keeps the newest
// calibrated image, and rendering of images to RGB with the current view
// settings.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "uti120/device.hpp"
#include "uti120/palette.hpp"
#include "uti120/process.hpp"
#include "uti120/radiometry.hpp"
#include "uti120/stream.hpp"

namespace uti120 {

// How images are shown.  rotation is clockwise in degrees (0, 90, 180, 270)
// and applied after mirror (left-right) and flip (upside-down).
struct View {
  std::string palette = "ironbow";
  bool mirror = false;
  bool flip = false;
  int rotation = 0;
};

struct RgbImage {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;  // packed RGB24, row-major
};

// Signal -> RGB with orientation, automatic gain (smoothed over successive
// calls) and a false-colour palette.  Not thread-safe; Pipeline serialises it.
class Renderer {
 public:
  explicit Renderer(const View& view = {});
  void set_view(const View& view);  // throws std::invalid_argument
  const View& view() const { return view_; }
  RgbImage operator()(const Plane& signal);

 private:
  View view_;
  palette::Lut lut_;
  AutoGain gain_;
};

struct Settings {
  int dark_frames = 16;
  double recalibrate_s = 0.0;      // periodic shutter recalibration, 0 = never
  std::FILE* raw_sink = nullptr;   // receives every raw frame if set (not owned)
  // With the camera's calibration, images carry temperatures (Image::temperature).
  std::optional<CameraCalibration> calibration = std::nullopt;
  RadiometrySettings radiometry = {};
};

class Pipeline {
 public:
  Pipeline(std::unique_ptr<Transport> transport, const Settings& settings = {},
           const View& view = {});
  ~Pipeline();
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Streaming, on-camera NUC and shutter calibration (blocks for ~4 s), then
  // the capture thread is started.  On failure the camera is left idle.
  // Throws std::logic_error if already started.  start() and stop() may be
  // called from different threads; stop() waits for a running start().
  void start();
  // Stops the capture thread and returns the camera to idle.  Idempotent.
  void stop();

  // The newest image, waiting at most `wait` for the first one; nullopt if
  // none arrived in time or the pipeline is stopped.  Rethrows a capture error.
  std::optional<Image> newest(std::chrono::milliseconds wait);
  // Mean signal of the next n captured images.
  // With radiometry, `temperature` (if given) receives the mean temperatures
  // of those of the n images that carry temperatures (all of them once the
  // pipeline has started: the start-up shutter frames are the reference).
  Plane snapshot(int n, Frame* last = nullptr, Plane* temperature = nullptr);
  // The capture thread recalibrates on the shutter before its next frame.
  void request_recalibration() { recalibrate_ = true; }

  // Thread-safe; applied by the capture thread before its next frame (or at
  // start()).  Without a calibration in Settings it only records the settings.
  void set_radiometry(const RadiometrySettings& settings);
  RadiometrySettings radiometry();
  bool radiometry_enabled() const { return radiometry_enabled_; }

  RgbImage render(const Plane& signal);
  void set_view(const View& view);
  View view();

  long frames_captured() const { return count_; }
  Camera& camera() { return cam_; }

 private:
  void run();
  void apply_radiometry();  // on the capture thread, or under lifecycle_ before it runs

  std::mutex lifecycle_;  // serialises start() and stop()
  Camera cam_;
  Stream stream_;
  std::mutex render_mutex_;
  Renderer renderer_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::optional<Image> latest_;
  std::exception_ptr error_;
  std::atomic<long> count_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> recalibrate_{false};
  bool streaming_ = false;
  std::thread thread_;

  const bool radiometry_enabled_;
  std::mutex radiometry_mutex_;
  RadiometrySettings radiometry_;
  bool radiometry_pending_ = false;
};

}  // namespace uti120

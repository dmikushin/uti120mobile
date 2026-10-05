#include "uti120/stream.hpp"

#include <chrono>

#include "uti120/log.hpp"

namespace uti120 {

static constexpr const char* LOG = "uti120.stream";

double monotonic_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Stream::Stream(Camera& cam, int dark_frames, double recalibrate_s, std::FILE* raw_sink)
    : cam_(cam), dark_frames_(dark_frames), recalibrate_s_(recalibrate_s), raw_sink_(raw_sink) {}

Frame Stream::grab() {
  Frame f = cam_.grab();
  if (raw_sink_ && std::fwrite(f.words.data(), 1, FRAME_BYTES, raw_sink_) != FRAME_BYTES)
    throw std::runtime_error("cannot write raw frame");
  if (radiometer_) last_temperature_ = radiometer_->feed(f);
  return f;
}

void Stream::enable_radiometry(const CameraCalibration& calibration,
                               const RadiometrySettings& settings) {
  radiometer_.emplace(calibration, settings);
  last_temperature_.reset();
}

void Stream::set_radiometry(const RadiometrySettings& settings) {
  if (radiometer_) radiometer_->set_settings(settings);
}

void Stream::start() {
  try {
    cam_.start();
    LOG_INFO(LOG, "streaming: %s", grab().describe().c_str());
    cam_.nuc();
    double end = monotonic_s() + NUC_SETTLE_S;
    while (monotonic_s() < end) grab();
    calibrate();
  } catch (...) {
    try {
      stop();
    } catch (const std::exception& e) {
      LOG_WARNING(LOG, "could not return the camera to idle: %s", e.what());
    }
    throw;
  }
}

void Stream::wait_shutter(bool closed) {
  bool reached = false;
  for (int i = 0; i < 30 && !reached; ++i) reached = grab().shutter_closed() == closed;
  if (!reached)
    throw DeviceError(std::string("shutter never reported ") + (closed ? "closed" : "open"));
  for (int i = 0; i < SHUTTER_SETTLE_FRAMES; ++i) grab();
}

void Stream::calibrate() {
  cam_.set_shutter(true);
  wait_shutter(true);
  std::vector<Frame> frames;
  for (int i = 0; i < dark_frames_; ++i) frames.push_back(grab());
  cam_.set_shutter(false);
  wait_shutter(false);
  calibration_ = Calibration::from_frames(frames);
  calibrated_at_ = monotonic_s();
  // Drift between the two halves of the shutter frames needs at least two frames.
  if (log_enabled(Level::Info) && frames.size() >= 2) {
    size_t half = frames.size() / 2;
    Plane drift = mean_pixels({frames.begin() + half, frames.end()});
    Plane first = mean_pixels({frames.begin(), frames.begin() + half});
    for (int i = 0; i < PIXELS; ++i) drift[i] -= first[i];
    Frame f = grab();
    LOG_INFO(LOG,
             "calibrated on %zu shutter frames, %d bad pixels, fpa %.2fC, "
             "row stripes: dark drift %.1f, first open frame %.1f",
             frames.size(), calibration_->bad_count(), frames.back().fpa_temp(),
             row_stripes(drift), row_stripes(calibration_->apply(f.pixels())));
  }
}

Image Stream::read() {
  if (recalibrate_s_ > 0 && monotonic_s() - calibrated_at_ > recalibrate_s_) calibrate();
  Frame f = grab();
  Plane signal = calibration_->apply(f.pixels());
  std::optional<Plane> temperature = std::move(last_temperature_);
  last_temperature_.reset();
  return {f, std::move(signal), std::move(temperature)};
}

void Stream::stop() { cam_.stop(); }

}  // namespace uti120

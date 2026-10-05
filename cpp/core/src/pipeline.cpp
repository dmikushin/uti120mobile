#include "uti120/pipeline.hpp"

#include <stdexcept>

#include "uti120/log.hpp"

namespace uti120 {

static constexpr const char* LOG = "uti120.pipeline";

Renderer::Renderer(const View& view) { set_view(view); }

void Renderer::set_view(const View& view) {
  if (view.rotation % 90 != 0)
    throw std::invalid_argument("rotation must be a multiple of 90 degrees");
  lut_ = palette::lut(view.palette);
  view_ = view;
  view_.rotation = ((view.rotation % 360) + 360) % 360;
}

RgbImage Renderer::operator()(const Plane& signal) {
  bool swap = view_.rotation == 90 || view_.rotation == 270;
  RgbImage out;
  out.width = swap ? HEIGHT : WIDTH;
  out.height = swap ? WIDTH : HEIGHT;
  Plane img(PIXELS);
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x) {
      // Undo the clockwise rotation to find the oriented sensor pixel...
      int ox, oy;
      switch (view_.rotation) {
        case 90: ox = y; oy = HEIGHT - 1 - x; break;
        case 180: ox = WIDTH - 1 - x; oy = HEIGHT - 1 - y; break;
        case 270: ox = WIDTH - 1 - y; oy = x; break;
        default: ox = x; oy = y; break;
      }
      // ...then undo mirror and flip.
      int sx = view_.mirror ? WIDTH - 1 - ox : ox;
      int sy = view_.flip ? HEIGHT - 1 - oy : oy;
      img[y * out.width + x] = signal[sy * WIDTH + sx];
    }
  out.rgb = palette::colorize(gain_(img), lut_);
  return out;
}

Pipeline::Pipeline(std::unique_ptr<Transport> transport, const Settings& settings,
                   const View& view)
    : cam_(std::move(transport)),
      stream_(cam_, settings.dark_frames, settings.recalibrate_s, settings.raw_sink),
      renderer_(view),
      radiometry_enabled_(settings.calibration.has_value()),
      radiometry_(settings.radiometry) {
  if (settings.calibration) stream_.enable_radiometry(*settings.calibration, settings.radiometry);
}

void Pipeline::set_radiometry(const RadiometrySettings& settings) {
  std::lock_guard lock(radiometry_mutex_);
  radiometry_ = settings;
  radiometry_pending_ = true;
}

RadiometrySettings Pipeline::radiometry() {
  std::lock_guard lock(radiometry_mutex_);
  return radiometry_;
}

void Pipeline::apply_radiometry() {
  std::lock_guard lock(radiometry_mutex_);
  if (!radiometry_pending_) return;
  radiometry_pending_ = false;
  stream_.set_radiometry(radiometry_);
}

Pipeline::~Pipeline() {
  try {
    stop();
  } catch (const std::exception& e) {
    LOG_WARNING(LOG, "could not return the camera to idle: %s", e.what());
  }
}

void Pipeline::start() {
  std::lock_guard life(lifecycle_);
  if (streaming_) throw std::logic_error("pipeline already started");
  apply_radiometry();  // the capture thread is not running yet
  stream_.start();  // returns the camera to idle itself if it throws
  {
    // A previous session's image, error or count must not leak into this one.
    std::lock_guard lock(mutex_);
    latest_.reset();
    error_ = nullptr;
    count_ = 0;
    stopping_ = false;
  }
  streaming_ = true;
  thread_ = std::thread([this] { run(); });
}

void Pipeline::stop() {
  std::lock_guard life(lifecycle_);
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cond_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (streaming_) {
    streaming_ = false;
    stream_.stop();
  }
}

void Pipeline::run() {
  try {
    while (!stopping_) {
      apply_radiometry();
      if (recalibrate_.exchange(false)) stream_.calibrate();
      Image im = stream_.read();
      std::lock_guard lock(mutex_);
      latest_ = std::move(im);
      ++count_;
      cond_.notify_all();
    }
  } catch (...) {
    std::lock_guard lock(mutex_);
    error_ = std::current_exception();
    cond_.notify_all();
  }
}

std::optional<Image> Pipeline::newest(std::chrono::milliseconds wait) {
  std::unique_lock lock(mutex_);
  cond_.wait_for(lock, wait, [&] { return latest_ || error_ || stopping_; });
  if (error_) std::rethrow_exception(error_);
  if (stopping_) return std::nullopt;
  return latest_;
}

Plane Pipeline::snapshot(int n, Frame* last, Plane* temperature) {
  std::vector<double> sum(PIXELS, 0.0), tsum(PIXELS, 0.0);
  int tcount = 0;
  std::unique_lock lock(mutex_);
  long seen = latest_ ? count_.load() : 0;
  for (int k = 0; k < n; ++k) {
    cond_.wait(lock, [&] { return count_ > seen || error_ || stopping_; });
    if (error_) std::rethrow_exception(error_);
    if (stopping_) throw std::runtime_error("pipeline stopped during snapshot");
    seen = count_;
    for (int i = 0; i < PIXELS; ++i) sum[i] += latest_->signal[i];
    if (latest_->temperature) {
      for (int i = 0; i < PIXELS; ++i) tsum[i] += (*latest_->temperature)[i];
      ++tcount;
    }
    if (last) *last = latest_->frame;
  }
  Plane mean(PIXELS);
  for (int i = 0; i < PIXELS; ++i) mean[i] = float(sum[i] / n);
  if (temperature) {
    temperature->clear();
    if (tcount) {
      temperature->resize(PIXELS);
      for (int i = 0; i < PIXELS; ++i) (*temperature)[i] = float(tsum[i] / tcount);
    }
  }
  return mean;
}

RgbImage Pipeline::render(const Plane& signal) {
  std::lock_guard lock(render_mutex_);
  return renderer_(signal);
}

void Pipeline::set_view(const View& view) {
  std::lock_guard lock(render_mutex_);
  renderer_.set_view(view);
}

View Pipeline::view() {
  std::lock_guard lock(render_mutex_);
  return renderer_.view();
}

}  // namespace uti120

// uti120 {info,snapshot,record,live}: one process from USB to MP4/PNG/window.
//
// Threads: a capture thread reads and calibrates camera frames as fast as the
// camera delivers them and keeps only the newest image; the main thread
// renders that image at a constant 25 Hz into the encoder or the window, so a
// slow consumer never delays frame requests and the video duration always
// equals the wall-clock duration.

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "device.hpp"
#include "log.hpp"
#include "media.hpp"
#include "palette.hpp"
#include "process.hpp"
#include "stream.hpp"

extern "C" {
#include <libavutil/log.h>
}

using namespace uti120;

namespace {

constexpr int VIDEO_FPS = 25;

std::atomic<bool> g_interrupted{false};
void on_signal(int) { g_interrupted = true; }

struct Options {
  std::string command;
  int verbose = 0;
  std::string palette = "ironbow";
  int scale = 4;
  bool mirror = false, flip = false;
  int dark_frames = 16;
  double recalibrate = 0.0;
  std::string output;
  int average = 8;
  std::string npy;
  double duration = -1;  // < 0: command default
  std::string raw;
};

[[noreturn]] void usage(int code) {
  std::FILE* out = code ? stderr : stdout;
  std::fprintf(out,
      "usage: uti120 [-v] COMMAND [options]\n"
      "\n"
      "UNI-T UTi120Mobile thermal camera.\n"
      "\n"
      "commands:\n"
      "  info                 print device information\n"
      "  snapshot             save one image (PNG)\n"
      "  record               record a video (H.264 MP4, %d fps)\n"
      "  live                 show live video in a window (Esc or q closes it)\n"
      "\n"
      "options for snapshot, record, live:\n"
      "  --palette NAME       grey, ironbow (default) or rainbow\n"
      "  --scale N            output upscaling factor (default 4)\n"
      "  --mirror             flip left-right\n"
      "  --flip               flip upside-down\n"
      "  --dark-frames N      closed-shutter frames averaged for calibration (default 16)\n"
      "  --recalibrate S      repeat the shutter calibration every S seconds (0 = never)\n"
      "snapshot:\n"
      "  -o, --output FILE    default uti120.png\n"
      "  --average N          frames averaged into the image (default 8)\n"
      "  --npy FILE           also save the offset-corrected signal as .npy\n"
      "record, live:\n"
      "  -o, --output FILE    record only; default uti120.mp4\n"
      "  -t, --duration S     seconds; default 10 for record, 0 (until closed) for live\n"
      "  --raw FILE           also write every raw 25600-byte frame to FILE\n"
      "\n"
      "  -v                   log progress (-vv: protocol debug)\n"
      "  -h, --help           this help\n",
      VIDEO_FPS);
  std::exit(code);
}

[[noreturn]] void fail(const std::string& msg) {
  std::fprintf(stderr, "uti120: %s\n", msg.c_str());
  std::exit(2);
}

double parse_number(const std::string& opt, const char* s, double min) {
  char* end = nullptr;
  double v = std::strtod(s, &end);
  if (!*s || *end || v < min) fail("invalid value for " + opt + ": " + s);
  return v;
}

Options parse_args(int argc, char** argv) {
  Options o;
  std::vector<std::string> args(argv + 1, argv + argc);
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= args.size()) fail("option " + a + " needs a value");
      return args[++i].c_str();
    };
    bool media = o.command == "snapshot" || o.command == "record" || o.command == "live";
    bool timed = o.command == "record" || o.command == "live";
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "-v" || a == "--verbose") o.verbose++;
    else if (a == "-vv") o.verbose += 2;
    else if (o.command.empty() && a[0] != '-') {
      if (a != "info" && a != "snapshot" && a != "record" && a != "live")
        fail("unknown command " + a);
      o.command = a;
    } else if (media && a == "--palette") o.palette = value();
    else if (media && a == "--scale") o.scale = int(parse_number(a, value(), 1));
    else if (media && a == "--mirror") o.mirror = true;
    else if (media && a == "--flip") o.flip = true;
    else if (media && a == "--dark-frames") o.dark_frames = int(parse_number(a, value(), 1));
    else if (media && a == "--recalibrate") o.recalibrate = parse_number(a, value(), 0);
    else if ((o.command == "snapshot" || o.command == "record") && (a == "-o" || a == "--output"))
      o.output = value();
    else if (o.command == "snapshot" && a == "--average") o.average = int(parse_number(a, value(), 1));
    else if (o.command == "snapshot" && a == "--npy") o.npy = value();
    else if (timed && (a == "-t" || a == "--duration")) o.duration = parse_number(a, value(), 0);
    else if (timed && a == "--raw") o.raw = value();
    else fail("unexpected argument " + a + (o.command.empty() ? "" : " for " + o.command));
  }
  if (o.command.empty()) usage(2);
  if (o.output.empty()) o.output = o.command == "snapshot" ? "uti120.png" : "uti120.mp4";
  if (o.duration < 0) o.duration = o.command == "record" ? 10.0 : 0.0;
  try {
    palette::lut(o.palette);
  } catch (const std::invalid_argument&) {
    fail("unknown palette " + o.palette + " (grey, ironbow, rainbow)");
  }
  return o;
}

// Signal -> RGB24 at sensor resolution.
class Renderer {
 public:
  explicit Renderer(const Options& o) : opt_(o), lut_(palette::lut(o.palette)) {}

  std::vector<uint8_t> operator()(const Plane& signal) {
    Plane img(PIXELS);
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x) {
        int sy = opt_.flip ? HEIGHT - 1 - y : y, sx = opt_.mirror ? WIDTH - 1 - x : x;
        img[y * WIDTH + x] = signal[sy * WIDTH + sx];
      }
    return palette::colorize(gain_(img), lut_);
  }

 private:
  const Options& opt_;
  palette::Lut lut_;
  AutoGain gain_;
};

// Reads the camera continuously and keeps only the newest image.
class Capture {
 public:
  explicit Capture(Stream& s) : stream_(s), thread_([this] { run(); }) {}
  ~Capture() { stop(); }

  // Blocks until the first image (or an error) is available.
  Image newest() {
    std::unique_lock lock(mutex_);
    cond_.wait(lock, [&] { return latest_ || error_; });
    if (error_) std::rethrow_exception(error_);
    return *latest_;
  }

  void stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
  }

  long count() const { return count_; }

 private:
  void run() {
    try {
      while (!stopping_) {
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

  Stream& stream_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::optional<Image> latest_;
  std::exception_ptr error_;
  std::atomic<long> count_{0};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

// Camera started and calibrated for the lifetime of the object; returned to
// idle on destruction.
class Session {
 public:
  explicit Session(const Options& o) : cam_(open_usb()) {
    if (!o.raw.empty()) {
      raw_ = std::fopen(o.raw.c_str(), "wb");
      if (!raw_) fail("cannot open " + o.raw + ": " + std::strerror(errno));
    }
    stream_.emplace(cam_, o.dark_frames, o.recalibrate, raw_);
    stream_->start();
  }
  ~Session() {
    try {
      stream_->stop();
    } catch (const std::exception& e) {
      LOG_WARNING("uti120", "could not return the camera to idle: %s", e.what());
    }
    if (raw_ && std::fclose(raw_) != 0) LOG_WARNING("uti120", "error closing raw file");
  }
  Stream& stream() { return *stream_; }

 private:
  Camera cam_;
  std::FILE* raw_ = nullptr;
  std::optional<Stream> stream_;
};

int cmd_info() {
  Camera cam(open_usb());
  DeviceInfo i = cam.info();
  std::printf("%-12s %s\n%-12s %s\n%-12s %s\n%-12s %u\n%-12s %u\n", "firmware",
              i.firmware.c_str(), "hardware", i.hardware.c_str(), "sensor", i.sensor.c_str(),
              "run_status", i.run_status, "init_status", i.init_status);
  return 0;
}

int cmd_snapshot(const Options& o) {
  Plane sum(PIXELS, 0.0f);
  std::optional<Frame> last;
  {
    Session s(o);
    for (int k = 0; k < o.average; ++k) {
      Image im = s.stream().read();
      for (int i = 0; i < PIXELS; ++i) sum[i] += im.signal[i];
      last = im.frame;
    }
  }
  for (float& v : sum) v /= float(o.average);
  Renderer render(o);
  write_png(o.output, render(sum).data(), WIDTH, HEIGHT, o.scale);
  if (!o.npy.empty()) write_npy(o.npy, sum, HEIGHT, WIDTH);
  std::vector<double> v(sum.begin(), sum.end());
  std::printf("%s: %s, signal p1/p50/p99 = [%.1f, %.1f, %.1f]\n", o.output.c_str(),
              last->describe().c_str(), percentile(v, 1), percentile(v, 50), percentile(v, 99));
  return 0;
}

// Feeds the newest image to `sink` VIDEO_FPS times a second.  The sink returns
// false to stop (window closed).
template <class Sink>
int pump(const Options& o, Sink&& sink) {
  Session session(o);
  Capture cap(session.stream());
  Renderer render(o);
  long ticks = 0;
  double t0 = monotonic_s();
  while (!g_interrupted && (o.duration == 0 || ticks < o.duration * VIDEO_FPS)) {
    if (!sink(render(cap.newest().signal).data())) break;
    ++ticks;
    double delay = t0 + double(ticks) / VIDEO_FPS - monotonic_s();
    if (delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
  }
  cap.stop();
  double dt = monotonic_s() - t0;
  std::printf("%ld video frames in %.1f s from %ld camera frames (%.1f fps)\n", ticks, dt,
              cap.count(), double(cap.count()) / dt);
  return 0;
}

int cmd_record(const Options& o) {
  VideoWriter video(o.output, WIDTH, HEIGHT, o.scale, VIDEO_FPS);
  int rc = pump(o, [&](const uint8_t* rgb) {
    video.write(rgb);
    return true;
  });
  video.finish();
  return rc;
}

int cmd_live(const Options& o) {
  Display display("UTi120Mobile", WIDTH, HEIGHT, o.scale);
  return pump(o, [&](const uint8_t* rgb) {
    display.show(rgb);
    return display.poll();
  });
}

}  // namespace

int main(int argc, char** argv) {
  Options o = parse_args(argc, argv);
  set_log_level(o.verbose >= 2 ? Level::Debug : o.verbose == 1 ? Level::Info : Level::Warning);
  // libav reports encoder statistics at its info level; show them with -vv only.
  av_log_set_level(o.verbose >= 2 ? AV_LOG_INFO : AV_LOG_ERROR);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  try {
    if (o.command == "info") return cmd_info();
    if (o.command == "snapshot") return cmd_snapshot(o);
    if (o.command == "record") return cmd_record(o);
    return cmd_live(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "uti120: %s\n", e.what());
    return 1;
  }
}

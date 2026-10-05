// uti120 {info,snapshot,record,live}: one process from USB to MP4/PNG/window.
//
// The backend's capture thread (uti120::Pipeline) reads and calibrates camera
// frames as fast as the camera delivers them and keeps only the newest image;
// the main thread renders that image at a constant 25 Hz into the encoder or
// the window, so a slow consumer never delays frame requests and the video
// duration always equals the wall-clock duration.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media.hpp"
#include "uti120/device.hpp"
#include "uti120/log.hpp"
#include "uti120/palette.hpp"
#include "uti120/pipeline.hpp"
#include "uti120/radiometry.hpp"

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
  bool celsius = false;  // measure temperatures (always for "log")
  RadiometrySettings radiometry;
  double interval = 1.0;  // log: seconds between rows
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
      "  log                  write min/max/mean/centre temperatures over time as CSV\n"
      "\n"
      "options for snapshot, record, live:\n"
      "  --palette NAME       grey, ironbow (default) or rainbow\n"
      "  --scale N            output upscaling factor (default 4)\n"
      "  --mirror             flip left-right\n"
      "  --flip               flip upside-down\n"
      "  --dark-frames N      closed-shutter frames averaged for calibration (default 16)\n"
      "  --recalibrate S      repeat the shutter calibration every S seconds (0 = never)\n"
      "temperatures (snapshot, live: with --celsius; log: always):\n"
      "  --celsius            measure temperatures; the first time, the camera's calibration\n"
      "                       is read and cached in ~/.cache/uti120, which restarts the camera\n"
      "  --emissivity E       of the target (default 0.95)\n"
      "  --distance M         to the target in metres (default 0.6)\n"
      "  --reflected C        reflected (ambient) temperature (default 23)\n"
      "  --high-range         high measuring range (above ~120 C)\n"
      "snapshot:\n"
      "  -o, --output FILE    default uti120.png\n"
      "  --average N          frames averaged into the image (default 8)\n"
      "  --npy FILE           also save the signal (with --celsius: temperatures, C) as .npy\n"
      "log:\n"
      "  -o, --output FILE    CSV file (default: standard output)\n"
      "  -t, --duration S     seconds (default 0: until interrupted)\n"
      "  --interval S         seconds between rows (default 1)\n"
      "record, live:\n"
      "  -o, --output FILE    record only; default uti120.mp4\n"
      "  -t, --duration S     seconds; default 10 for record, 0 (until closed) for live\n"
      "  --raw FILE           also write every raw 25600-byte frame to FILE\n"
      "\n"
      "  -v                   log progress (-vv: protocol debug)\n"
      "  -h, --help           this help\n"
      "  --version            print the version\n",
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
    bool media = o.command == "snapshot" || o.command == "record" || o.command == "live" ||
                 o.command == "log";
    bool timed = o.command == "record" || o.command == "live" || o.command == "log";
    bool measure = o.command == "snapshot" || o.command == "live" || o.command == "log";
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--version") {
      std::printf("uti120 %s\n", UTI120_VERSION);
      std::exit(0);
    }
    else if (a == "-v" || a == "--verbose") o.verbose++;
    else if (a == "-vv") o.verbose += 2;
    else if (o.command.empty() && a[0] != '-') {
      if (a != "info" && a != "snapshot" && a != "record" && a != "live" && a != "log")
        fail("unknown command " + a);
      o.command = a;
    } else if (media && a == "--palette") o.palette = value();
    else if (media && a == "--scale") o.scale = int(parse_number(a, value(), 1));
    else if (media && a == "--mirror") o.mirror = true;
    else if (media && a == "--flip") o.flip = true;
    else if (media && a == "--dark-frames") o.dark_frames = int(parse_number(a, value(), 1));
    else if (media && a == "--recalibrate") o.recalibrate = parse_number(a, value(), 0);
    else if (measure && a == "--celsius") o.celsius = true;
    else if (measure && a == "--emissivity") o.radiometry.emissivity = float(parse_number(a, value(), 0.01));
    else if (measure && a == "--distance") o.radiometry.distance = float(parse_number(a, value(), 0));
    else if (measure && a == "--reflected") o.radiometry.reflected = float(parse_number(a, value(), -100));
    else if (measure && a == "--high-range") o.radiometry.high_range = true;
    else if (o.command == "log" && a == "--interval") o.interval = parse_number(a, value(), 0.01);
    else if ((o.command == "snapshot" || o.command == "record" || o.command == "log") &&
             (a == "-o" || a == "--output"))
      o.output = value();
    else if (o.command == "snapshot" && a == "--average") o.average = int(parse_number(a, value(), 1));
    else if (o.command == "snapshot" && a == "--npy") o.npy = value();
    else if (timed && (a == "-t" || a == "--duration")) o.duration = parse_number(a, value(), 0);
    else if (timed && o.command != "log" && a == "--raw") o.raw = value();
    else fail("unexpected argument " + a + (o.command.empty() ? "" : " for " + o.command));
  }
  if (o.command.empty()) usage(2);
  if (o.output.empty())
    o.output = o.command == "snapshot" ? "uti120.png" : o.command == "log" ? "-" : "uti120.mp4";
  if (o.duration < 0) o.duration = o.command == "record" ? 10.0 : 0.0;
  if (o.command == "log") o.celsius = true;
  if (o.radiometry.emissivity > 1) fail("emissivity must be at most 1");
  try {
    palette::lut(o.palette);
  } catch (const std::invalid_argument&) {
    fail("unknown palette " + o.palette + " (grey, ironbow, rainbow)");
  }
  return o;
}

// The raw frame file, when requested, outlives the pipeline that writes it.
struct FileCloser {
  void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File open_raw(const Options& o) {
  if (o.raw.empty()) return nullptr;
  File f(std::fopen(o.raw.c_str(), "wb"));
  if (!f) fail("cannot open " + o.raw + ": " + std::strerror(errno));
  return f;
}

std::string calibration_cache(const std::string& sensor) {
  // A sensor id names a directory; refuse anything that could escape it.
  if (sensor.empty() || sensor.find_first_not_of(
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
                            std::string::npos)
    fail("unexpected sensor id \"" + sensor + "\"");
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  std::string base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.cache";
  return base + "/uti120/" + sensor;
}

// Reboots the camera and waits until it is back: first until the old device
// has left the bus (it keeps answering, in its old state, for about 1.0 s after
// the command), then until the new one reports init_status 1 (about 3.3 s);
// it must be the same sensor.  Measured timings: see usb_camera_address().
void reboot_and_wait(Camera& cam, const std::string& expected) {
  int old_address = usb_camera_address();
  cam.reboot();
  double deadline = monotonic_s() + 10;
  while (usb_camera_address() == old_address) {
    if (monotonic_s() > deadline) fail("the camera did not restart");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (deadline = monotonic_s() + 15;;) {
    try {
      DeviceInfo info = Camera(open_usb()).info();
      if (info.init_status == 1) {
        if (info.sensor != expected)
          throw WrongCamera("after the restart a different camera answered (" + info.sensor +
                            ", expected " + expected + ")");
        return;
      }
    } catch (const WrongCamera&) {
      throw;
    } catch (const DeviceError& e) {
      if (monotonic_s() > deadline) fail(std::string("camera did not come back after restart: ") + e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Restarts a camera that is stuck (CameraStuck).
void restart_camera(const char* why) {
  Camera cam(open_usb());
  std::string sensor = cam.info().sensor;
  std::fprintf(stderr, "uti120: %s; restarting the camera\n", why);
  reboot_and_wait(cam, sensor);
}

// Runs `f`; if the camera turns out to be silent, restarts it and runs `f` once more.
template <class F>
auto with_restart(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const CameraStuck& e) {
    restart_camera(e.what());
    return f();
  }
}

// The camera's calibration for temperatures: read from the camera once and
// cached per sensor (same files as the Python tool).  Reading it stops the
// camera's frame stream until it is rebooted, so the camera is then rebooted
// and this returns once it is back.
CameraCalibration camera_calibration() {
  std::string dir, sensor;
  {
    Camera cam(open_usb());
    sensor = cam.info().sensor;
    dir = calibration_cache(sensor);
    if (std::filesystem::exists(dir + "/coefficients.json")) {
      LOG_INFO("uti120", "calibration from %s", dir.c_str());
      CameraCalibration cal = CameraCalibration::load(dir);
      cal.validate(sensor);
      return cal;
    }
  }
  std::fprintf(stderr, "uti120: reading the camera's calibration (once; cached in %s)\n", dir.c_str());
  CameraCalibration cal = with_restart([&] {
    Camera cam(open_usb());
    CameraCalibration c = read_camera_calibration(cam);
    // The firmware sends no frames after an upload until it is restarted.
    reboot_and_wait(cam, sensor);
    return c;
  });
  std::filesystem::create_directories(dir);
  cal.save(dir);
  return cal;
}

std::unique_ptr<Pipeline> start_pipeline(const Options& o, std::FILE* raw) {
  View view;
  view.palette = o.palette;
  view.mirror = o.mirror;
  view.flip = o.flip;
  Settings settings{o.dark_frames, o.recalibrate, raw};
  if (o.celsius) {
    settings.calibration = camera_calibration();
    settings.radiometry = o.radiometry;
  }
  return with_restart([&] {
    auto p = std::make_unique<Pipeline>(open_usb(), settings, view);
    p->start();
    return p;
  });
}

struct Summary {
  float min, max, mean, centre;
};

Summary summarize(const Plane& t) {
  double sum = 0;
  float lo = t[0], hi = t[0];
  for (float v : t) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    sum += v;
  }
  return {lo, hi, float(sum / double(t.size())), t[(HEIGHT / 2) * WIDTH + WIDTH / 2]};
}

int cmd_info() {
  Camera cam(open_usb());
  DeviceInfo i = cam.info();
  std::printf("%-12s %s\n%-12s %s\n%-12s %s\n%-12s %u\n%-12s %u\n", "firmware",
              i.firmware.c_str(), "hardware", i.hardware.c_str(), "sensor", i.sensor.c_str(),
              "run_status", i.run_status, "init_status", i.init_status);
  return 0;
}

int cmd_snapshot(const Options& o) {
  Frame last;
  Plane mean, temperature;
  RgbImage img;
  {
    auto p = start_pipeline(o, nullptr);
    mean = p->snapshot(o.average, &last, &temperature);
    img = p->render(mean);
  }
  write_png(o.output, img.rgb.data(), img.width, img.height, o.scale);
  if (o.celsius) {
    if (temperature.empty()) fail("no temperatures were measured");
    Summary s = summarize(temperature);
    std::printf("temperature: min %.1f C, max %.1f C, mean %.1f C, centre %.1f C\n", s.min, s.max,
                s.mean, s.centre);
  }
  if (!o.npy.empty()) write_npy(o.npy, o.celsius ? temperature : mean, HEIGHT, WIDTH);
  std::vector<double> v(mean.begin(), mean.end());
  std::printf("%s: %s, signal p1/p50/p99 = [%.1f, %.1f, %.1f]\n", o.output.c_str(),
              last.describe().c_str(), percentile(v, 1), percentile(v, 50), percentile(v, 99));
  return 0;
}

// Feeds the newest image to `sink` VIDEO_FPS times a second.  The sink is
// created from the size of the rendered image and returns false to stop
// (window closed).
template <class MakeSink>
int pump(const Options& o, MakeSink&& make_sink) {
  File raw = open_raw(o);
  long ticks = 0, captured = 0;
  double t0 = 0, dt = 0;
  {
    auto p = start_pipeline(o, raw.get());
    std::optional<Image> im;
    while (!g_interrupted && !(im = p->newest(std::chrono::milliseconds(50)))) {
    }
    if (im) {
      RgbImage first = p->render(im->signal);
      auto sink = make_sink(first.width, first.height);
      t0 = monotonic_s();
      while (!g_interrupted && (o.duration == 0 || ticks < o.duration * VIDEO_FPS)) {
        im = p->newest(std::chrono::milliseconds(50));
        if (!im) continue;
        if (!sink(p->render(im->signal).rgb.data(), *im)) break;
        ++ticks;
        double delay = t0 + double(ticks) / VIDEO_FPS - monotonic_s();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
      }
      dt = monotonic_s() - t0;
    }
    captured = p->frames_captured();
  }
  if (raw && std::fclose(raw.release()) != 0) fail("error writing " + o.raw);
  std::printf("%ld video frames in %.1f s from %ld camera frames (%.1f fps)\n", ticks, dt,
              captured, dt > 0 ? double(captured) / dt : 0.0);
  return 0;
}

int cmd_record(const Options& o) {
  std::unique_ptr<VideoWriter> video;
  int rc = pump(o, [&](int w, int h) {
    video = std::make_unique<VideoWriter>(o.output, w, h, o.scale, VIDEO_FPS);
    return [&](const uint8_t* rgb, const Image&) {
      video->write(rgb);
      return true;
    };
  });
  if (video) video->finish();
  return rc;
}

int cmd_live(const Options& o) {
  std::unique_ptr<Display> display;
  return pump(o, [&](int w, int h) {
    display = std::make_unique<Display>("UTi120Mobile", w, h, o.scale);
    return [&](const uint8_t* rgb, const Image& im) {
      display->show(rgb);
      if (im.temperature) {
        Summary s = summarize(*im.temperature);
        char title[96];
        std::snprintf(title, sizeof title, "UTi120Mobile  centre %.1f C  min %.1f C  max %.1f C",
                      s.centre, s.min, s.max);
        display->set_title(title);
      }
      return display->poll();
    };
  });
}

// One CSV row per interval: for watching a GPU or anything else heat up.
int cmd_log(const Options& o) {
  File file;
  std::FILE* out = stdout;
  if (o.output != "-") {
    file.reset(std::fopen(o.output.c_str(), "w"));
    if (!file) fail("cannot open " + o.output + ": " + std::strerror(errno));
    out = file.get();
  }
  std::fprintf(out, "time_s,min_c,max_c,mean_c,centre_c,camera_fpa_c\n");
  auto p = start_pipeline(o, nullptr);
  double t0 = monotonic_s(), next = t0;
  long last_count = -1;
  while (!g_interrupted && (o.duration == 0 || monotonic_s() - t0 < o.duration)) {
    std::optional<Image> im = p->newest(std::chrono::milliseconds(100));
    if (!im || !im->temperature || p->frames_captured() == last_count || monotonic_s() < next)
      continue;
    last_count = p->frames_captured();
    Summary s = summarize(*im->temperature);
    std::fprintf(out, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", monotonic_s() - t0, s.min, s.max, s.mean,
                 s.centre, im->frame.fpa_temp());
    std::fflush(out);
    next += o.interval;
  }
  return 0;
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
    if (o.command == "log") return cmd_log(o);
    return cmd_live(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "uti120: %s\n", e.what());
    return 1;
  }
}

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
    bool media = o.command == "snapshot" || o.command == "record" || o.command == "live";
    bool timed = o.command == "record" || o.command == "live";
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--version") {
      std::printf("uti120 %s\n", UTI120_VERSION);
      std::exit(0);
    }
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

std::unique_ptr<Pipeline> start_pipeline(const Options& o, std::FILE* raw) {
  View view;
  view.palette = o.palette;
  view.mirror = o.mirror;
  view.flip = o.flip;
  auto p = std::make_unique<Pipeline>(open_usb(), Settings{o.dark_frames, o.recalibrate, raw}, view);
  p->start();
  return p;
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
  Plane mean;
  RgbImage img;
  {
    auto p = start_pipeline(o, nullptr);
    mean = p->snapshot(o.average, &last);
    img = p->render(mean);
  }
  write_png(o.output, img.rgb.data(), img.width, img.height, o.scale);
  if (!o.npy.empty()) write_npy(o.npy, mean, HEIGHT, WIDTH);
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
        if (!sink(p->render(im->signal).rgb.data())) break;
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
    return [&](const uint8_t* rgb) {
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
    return [&](const uint8_t* rgb) {
      display->show(rgb);
      return display->poll();
    };
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

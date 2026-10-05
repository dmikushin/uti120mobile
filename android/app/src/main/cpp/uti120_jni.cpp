// JNI bridge between the Kotlin UI (io.github.dmikushin.uti120.NativeCamera)
// and the shared backend (uti120::Pipeline).  Every native failure is turned
// into a java.lang.RuntimeException carrying the backend's message.

#include <android/bitmap.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "uti120/log.hpp"
#include "uti120/palette.hpp"
#include "uti120/pipeline.hpp"
#include "uti120/radiometry.hpp"
#include "uti120/replay.hpp"

using namespace uti120;

namespace {

Pipeline* from_handle(jlong h) { return reinterpret_cast<Pipeline*>(h); }

void throw_java(JNIEnv* env, const char* msg) {
  if (env->ExceptionCheck()) return;
  jclass cls = env->FindClass("java/lang/RuntimeException");
  env->ThrowNew(cls, msg);
}

template <class F>
auto guarded(JNIEnv* env, F&& f, decltype(f()) fallback) -> decltype(f()) {
  try {
    return f();
  } catch (const std::exception& e) {
    throw_java(env, e.what());
  } catch (...) {
    throw_java(env, "unknown native error");
  }
  return fallback;
}

// Throws if the string is null or cannot be read (a Java exception is then pending).
std::string to_string(JNIEnv* env, jstring s) {
  if (!s) throw std::invalid_argument("null string");
  const char* c = env->GetStringUTFChars(s, nullptr);
  if (!c) throw std::runtime_error("cannot read Java string");
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

// Copies packed RGB24 into an ARGB_8888 bitmap of exactly the same size.
bool copy_into(JNIEnv* env, jobject bitmap, const RgbImage& img) {
  AndroidBitmapInfo info;
  if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || int(info.width) != img.width ||
      int(info.height) != img.height)
    throw std::runtime_error("bitmap must be RGBA_8888 of " + std::to_string(img.width) + "x" +
                             std::to_string(img.height));
  void* pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
    throw std::runtime_error("cannot lock bitmap pixels");
  for (int y = 0; y < img.height; ++y) {
    auto* row = static_cast<uint8_t*>(pixels) + size_t(y) * info.stride;
    const uint8_t* src = img.rgb.data() + size_t(y) * img.width * 3;
    for (int x = 0; x < img.width; ++x) {
      row[4 * x] = src[3 * x];
      row[4 * x + 1] = src[3 * x + 1];
      row[4 * x + 2] = src[3 * x + 2];
      row[4 * x + 3] = 0xFF;
    }
  }
  AndroidBitmap_unlockPixels(env, bitmap);
  return true;
}

// Temperature summary of one image in display coordinates (the view's
// orientation, pixel units of the rendered image), as the Kotlin side reads it:
//   {valid, centre, min, min_x, min_y, max, max_x, max_y}
// Like the vendor app (UsbCameraHelper.callBackOneFrameBitmap), each spot value
// is the mean over the 3x3 neighbourhood of the pixel, cut at the image edges;
// the extremes are the hottest and coldest pixels.
constexpr int SUMMARY_LEN = 8;

void summarize(const Plane& temperature, const View& view, float* out) {
  bool swap = view.rotation == 90 || view.rotation == 270;
  int w = swap ? HEIGHT : WIDTH, h = swap ? WIDTH : HEIGHT;
  // The same mapping as Renderer: display pixel -> sensor pixel.
  std::vector<float> t(size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int ox, oy;
      switch (view.rotation) {
        case 90: ox = y; oy = HEIGHT - 1 - x; break;
        case 180: ox = WIDTH - 1 - x; oy = HEIGHT - 1 - y; break;
        case 270: ox = WIDTH - 1 - y; oy = x; break;
        default: ox = x; oy = y; break;
      }
      int sx = view.mirror ? WIDTH - 1 - ox : ox;
      int sy = view.flip ? HEIGHT - 1 - oy : oy;
      t[size_t(y) * w + x] = temperature[sy * WIDTH + sx];
    }
  auto spot = [&](int cx, int cy) {
    double sum = 0;
    int n = 0;
    for (int y = std::max(0, cy - 1); y <= std::min(h - 1, cy + 1); ++y)
      for (int x = std::max(0, cx - 1); x <= std::min(w - 1, cx + 1); ++x) {
        sum += t[size_t(y) * w + x];
        ++n;
      }
    return float(sum / n);
  };
  size_t lo = 0, hi = 0;
  for (size_t i = 1; i < t.size(); ++i) {
    if (t[i] < t[lo]) lo = i;
    if (t[i] > t[hi]) hi = i;
  }
  int lx = int(lo % w), ly = int(lo / w), hx = int(hi % w), hy = int(hi / w);
  float v[SUMMARY_LEN] = {1.0f, spot(w / 2, h / 2), spot(lx, ly), float(lx), float(ly),
                          spot(hx, hy), float(hx), float(hy)};
  std::copy(v, v + SUMMARY_LEN, out);
}

void write_summary(JNIEnv* env, jfloatArray dst, const std::optional<Plane>& temperature,
                   const View& view) {
  if (!dst) return;
  if (env->GetArrayLength(dst) < SUMMARY_LEN) throw std::invalid_argument("summary array too short");
  float v[SUMMARY_LEN] = {};
  if (temperature && !temperature->empty()) summarize(*temperature, view, v);
  env->SetFloatArrayRegion(dst, 0, SUMMARY_LEN, v);
}

}  // namespace

extern "C" {

// The camera is the USB device behind `fd` or, if `replay` is not null, the
// raw frames recorded in that file (for testing without the device).  With a
// calibration directory (CameraCalibration::save layout), images carry
// temperatures.
JNIEXPORT jlong JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeOpen(
    JNIEnv* env, jclass, jint fd, jstring replay, jint dark_frames, jdouble recalibrate_s,
    jstring palette, jboolean mirror, jboolean flip, jint rotation, jstring calibration_dir,
    jfloat emissivity, jfloat reflected, jfloat distance, jboolean high_range) {
  return guarded(env, [&]() -> jlong {
    // Start-up and calibration messages are rare and useful in logcat.
    set_log_level(Level::Info);
    View view{to_string(env, palette), bool(mirror), bool(flip), int(rotation)};
    Settings settings{int(dark_frames), double(recalibrate_s), nullptr};
    if (calibration_dir) {
      settings.calibration = CameraCalibration::load(to_string(env, calibration_dir));
      settings.radiometry = {float(emissivity), float(reflected), float(distance), bool(high_range)};
    }
    auto transport = replay ? open_replay(to_string(env, replay)) : open_usb_fd(fd);
    auto p = std::make_unique<Pipeline>(std::move(transport), settings, view);
    return reinterpret_cast<jlong>(p.release());
  }, jlong(0));
}

// Checks a cached calibration against the camera's sensor id: null if it
// belongs to this camera, otherwise why not (CameraCalibration::validate).
JNIEXPORT jstring JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeCheckCalibration(
    JNIEnv* env, jclass, jstring dir, jstring sensor) {
  try {
    CameraCalibration::load(to_string(env, dir)).validate(to_string(env, sensor));
    return nullptr;
  } catch (const std::exception& e) {
    return env->NewStringUTF(e.what());
  }
}

// The camera's sensor id (system registers 7..11), which names its calibration cache.
JNIEXPORT jstring JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeSensorId(
    JNIEnv* env, jclass, jint fd) {
  return guarded(env, [&]() -> jstring {
    Camera cam(open_usb_fd(fd));
    return env->NewStringUTF(cam.info().sensor.c_str());
  }, static_cast<jstring>(nullptr));
}

// The sensor id if the camera behind `fd` answers and has finished starting
// up (init status 1), else null.  Never throws: used to poll for the camera
// after its restart.
JNIEXPORT jstring JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeProbe(
    JNIEnv* env, jclass, jint fd) {
  try {
    Camera cam(open_usb_fd(fd));
    DeviceInfo info = cam.info();
    return info.init_status == 1 ? env->NewStringUTF(info.sensor.c_str()) : nullptr;
  } catch (const std::exception& e) {
    LOG_DEBUG("uti120.jni", "probe: %s", e.what());
    return nullptr;
  }
}

// Reads the camera's calibration into `dir` and reboots the camera: after a
// flash read the firmware delivers no frames until it restarts.  The camera
// then leaves the bus and comes back as a new device; the caller closes `fd`.
JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeReadCalibration(
    JNIEnv* env, jclass, jint fd, jstring dir) {
  guarded(env, [&] {
    std::string path = to_string(env, dir);
    Camera cam(open_usb_fd(fd));
    read_camera_calibration(cam).save(path);
    try {
      cam.write_reg(SYS_WRITE, REG_REBOOT, 1);
    } catch (const DeviceError&) {
      // the camera may go away before it answers
    }
    return 0;
  }, 0);
}

JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeSetRadiometry(
    JNIEnv* env, jclass, jlong h, jfloat emissivity, jfloat reflected, jfloat distance,
    jboolean high_range) {
  guarded(env, [&] {
    from_handle(h)->set_radiometry({float(emissivity), float(reflected), float(distance), bool(high_range)});
    return 0;
  }, 0);
}

JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeStart(JNIEnv* env,
                                                                                 jclass, jlong h) {
  guarded(env, [&] { from_handle(h)->start(); return 0; }, 0);
}

JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeClose(JNIEnv* env,
                                                                                 jclass, jlong h) {
  guarded(env, [&] { delete from_handle(h); return 0; }, 0);
}

// Size of rendered images with the current view: {width, height}.
JNIEXPORT jintArray JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeImageSize(
    JNIEnv* env, jclass, jlong h) {
  int r = from_handle(h)->view().rotation;
  jint size[2] = {r % 180 ? HEIGHT : WIDTH, r % 180 ? WIDTH : HEIGHT};
  jintArray out = env->NewIntArray(2);
  env->SetIntArrayRegion(out, 0, 2, size);
  return out;
}

// Renders the newest image into `bitmap` and its temperature summary into
// `summary` (see summarize; valid = 0 without temperatures); returns false if
// there is no image yet (or the bitmap size no longer matches the view).
JNIEXPORT jboolean JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeRender(
    JNIEnv* env, jclass, jlong h, jobject bitmap, jfloatArray summary) {
  return guarded(env, [&]() -> jboolean {
    Pipeline* p = from_handle(h);
    auto im = p->newest(std::chrono::milliseconds(100));
    if (!im) return JNI_FALSE;
    RgbImage img = p->render(im->signal);
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
      throw std::runtime_error("cannot read bitmap info");
    if (int(info.width) != img.width || int(info.height) != img.height) return JNI_FALSE;
    write_summary(env, summary, im->temperature, p->view());
    return copy_into(env, bitmap, img) ? JNI_TRUE : JNI_FALSE;
  }, jboolean(JNI_FALSE));
}

// Average of the next n images, rendered into `bitmap`, with the summary of
// their mean temperatures.
JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeSnapshot(
    JNIEnv* env, jclass, jlong h, jint n, jobject bitmap, jfloatArray summary) {
  guarded(env, [&] {
    Pipeline* p = from_handle(h);
    Plane temperature;
    copy_into(env, bitmap, p->render(p->snapshot(int(n), nullptr, &temperature)));
    write_summary(env, summary, temperature.empty() ? std::nullopt : std::optional<Plane>(temperature),
                  p->view());
    return 0;
  }, 0);
}

JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeSetView(
    JNIEnv* env, jclass, jlong h, jstring palette, jboolean mirror, jboolean flip,
    jint rotation) {
  guarded(env, [&] {
    from_handle(h)->set_view({to_string(env, palette), bool(mirror), bool(flip), int(rotation)});
    return 0;
  }, 0);
}

JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeRecalibrate(
    JNIEnv* env, jclass, jlong h) {
  guarded(env, [&] { from_handle(h)->request_recalibration(); return 0; }, 0);
}

// Cumulative frame timing of the camera, see uti120::GrabStats:
// {frames, ignored, dropped, reads, drain_ms, first_ms, transfer_ms, total_ms}.
JNIEXPORT jdoubleArray JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeStats(
    JNIEnv* env, jclass, jlong h) {
  return guarded(env, [&]() -> jdoubleArray {
    GrabStats s = from_handle(h)->camera().stats();
    jdouble v[8] = {double(s.frames), double(s.ignored), double(s.dropped), double(s.reads),
                    s.drain_ms, s.first_ms, s.transfer_ms, s.total_ms};
    jdoubleArray out = env->NewDoubleArray(8);
    if (!out) throw std::runtime_error("out of memory");
    env->SetDoubleArrayRegion(out, 0, 8, v);
    return out;
  }, static_cast<jdoubleArray>(nullptr));
}

JNIEXPORT jobjectArray JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativePaletteNames(
    JNIEnv* env, jclass) {
  jobjectArray out = env->NewObjectArray(std::size(palette::NAMES),
                                         env->FindClass("java/lang/String"), nullptr);
  for (size_t i = 0; i < std::size(palette::NAMES); ++i)
    env->SetObjectArrayElement(out, jsize(i), env->NewStringUTF(palette::NAMES[i]));
  return out;
}

// The palette's 256 colours as ARGB ints, for drawing swatches.
JNIEXPORT jintArray JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativePaletteColors(
    JNIEnv* env, jclass, jstring name) {
  return guarded(env, [&]() -> jintArray {
    palette::Lut lut = palette::lut(to_string(env, name));
    jint colors[256];
    for (int i = 0; i < 256; ++i)
      colors[i] = jint(0xFF000000u | uint32_t(lut[i][0]) << 16 | uint32_t(lut[i][1]) << 8 |
                       uint32_t(lut[i][2]));
    jintArray out = env->NewIntArray(256);
    env->SetIntArrayRegion(out, 0, 256, colors);
    return out;
  }, static_cast<jintArray>(nullptr));
}

}  // extern "C"

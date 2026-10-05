// JNI bridge between the Kotlin UI (io.github.dmikushin.uti120.NativeCamera)
// and the shared backend (uti120::Pipeline).  Every native failure is turned
// into a java.lang.RuntimeException carrying the backend's message.

#include <android/bitmap.h>
#include <jni.h>

#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "uti120/log.hpp"
#include "uti120/palette.hpp"
#include "uti120/pipeline.hpp"

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

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeOpen(
    JNIEnv* env, jclass, jint fd, jint dark_frames, jdouble recalibrate_s, jstring palette,
    jboolean mirror, jboolean flip, jint rotation) {
  return guarded(env, [&]() -> jlong {
    // Start-up and calibration messages are rare and useful in logcat.
    set_log_level(Level::Info);
    View view{to_string(env, palette), bool(mirror), bool(flip), int(rotation)};
    auto p = std::make_unique<Pipeline>(open_usb_fd(fd),
                                        Settings{int(dark_frames), double(recalibrate_s), nullptr},
                                        view);
    return reinterpret_cast<jlong>(p.release());
  }, jlong(0));
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

// Renders the newest image into `bitmap`; returns false if there is no image
// yet (or the bitmap size no longer matches the view, after a rotation).
JNIEXPORT jboolean JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeRender(
    JNIEnv* env, jclass, jlong h, jobject bitmap) {
  return guarded(env, [&]() -> jboolean {
    Pipeline* p = from_handle(h);
    auto im = p->newest(std::chrono::milliseconds(100));
    if (!im) return JNI_FALSE;
    RgbImage img = p->render(im->signal);
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
      throw std::runtime_error("cannot read bitmap info");
    if (int(info.width) != img.width || int(info.height) != img.height) return JNI_FALSE;
    return copy_into(env, bitmap, img) ? JNI_TRUE : JNI_FALSE;
  }, jboolean(JNI_FALSE));
}

// Average of the next n images, rendered into `bitmap`.
JNIEXPORT void JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeSnapshot(
    JNIEnv* env, jclass, jlong h, jint n, jobject bitmap) {
  guarded(env, [&] {
    Pipeline* p = from_handle(h);
    copy_into(env, bitmap, p->render(p->snapshot(int(n))));
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
    JNIEnv*, jclass, jlong h) {
  from_handle(h)->request_recalibration();
}

JNIEXPORT jlong JNICALL Java_io_github_dmikushin_uti120_NativeCamera_nativeFramesCaptured(
    JNIEnv*, jclass, jlong h) {
  return from_handle(h)->frames_captured();
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

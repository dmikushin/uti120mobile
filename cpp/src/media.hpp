// In-process media sinks: upscaling, H.264/MP4 recording, PNG, live window.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
struct AVFrame;
struct SwsContext;
}

namespace uti120 {

struct MediaError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Throws MediaError with the libav error text if rc < 0.
int check_av(int rc, const char* what);

struct AVFrameDeleter {
  void operator()(AVFrame* f) const;
};
using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// Bicubic upscaling of packed RGB24 images to a target pixel format.
class Scaler {
 public:
  Scaler(int src_w, int src_h, int dst_w, int dst_h, int dst_format);
  ~Scaler();
  Scaler(const Scaler&) = delete;
  Scaler& operator=(const Scaler&) = delete;

  // Writes into dst (allocated by the caller with matching size and format).
  void scale(const uint8_t* rgb, AVFrame* dst);
  FramePtr alloc_frame() const;

  int dst_w, dst_h, dst_format;

 private:
  int src_w_, src_h_;
  SwsContext* sws_ = nullptr;
};

// H.264 in MP4 at a constant frame rate.
class VideoWriter {
 public:
  VideoWriter(const std::string& path, int src_w, int src_h, int scale, int fps);
  ~VideoWriter();
  VideoWriter(const VideoWriter&) = delete;
  VideoWriter& operator=(const VideoWriter&) = delete;

  void write(const uint8_t* rgb);
  // Flushes the encoder and writes the MP4 trailer; called by the destructor
  // if not called explicitly (errors are then only logged).
  void finish();
  int64_t frames() const { return pts_; }

 private:
  void drain(bool flush);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  int64_t pts_ = 0;
  bool finished_ = false;
};

// Encodes an RGB24 image (upscaled by `scale`) as PNG.
void write_png(const std::string& path, const uint8_t* rgb, int w, int h, int scale);

// Little-endian float32 array in NumPy's .npy format.
void write_npy(const std::string& path, const std::vector<float>& data, int rows, int cols);

// Live window.  Shows upscaled RGB24 frames; closed by the window button, Esc or q.
class Display {
 public:
  Display(const std::string& title, int src_w, int src_h, int scale);
  ~Display();
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  void show(const uint8_t* rgb);
  // Handles pending window events; returns false once the user closed the window.
  bool poll();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uti120

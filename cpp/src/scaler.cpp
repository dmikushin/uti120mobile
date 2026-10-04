#include <cstdio>

#include "media.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace uti120 {

int check_av(int rc, const char* what) {
  if (rc < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, err, sizeof err);
    throw MediaError(std::string(what) + ": " + err);
  }
  return rc;
}

void AVFrameDeleter::operator()(AVFrame* f) const { av_frame_free(&f); }

Scaler::Scaler(int src_w, int src_h, int dst_w_, int dst_h_, int dst_format_)
    : dst_w(dst_w_), dst_h(dst_h_), dst_format(dst_format_), src_w_(src_w), src_h_(src_h) {
  sws_ = sws_getContext(src_w, src_h, AV_PIX_FMT_RGB24, dst_w, dst_h,
                        static_cast<AVPixelFormat>(dst_format), SWS_BICUBIC, nullptr, nullptr,
                        nullptr);
  if (!sws_) throw MediaError("sws_getContext failed");
}

Scaler::~Scaler() { sws_freeContext(sws_); }

FramePtr Scaler::alloc_frame() const {
  FramePtr f(av_frame_alloc());
  if (!f) throw MediaError("av_frame_alloc failed");
  f->format = dst_format;
  f->width = dst_w;
  f->height = dst_h;
  check_av(av_frame_get_buffer(f.get(), 0), "av_frame_get_buffer");
  return f;
}

void Scaler::scale(const uint8_t* rgb, AVFrame* dst) {
  check_av(av_frame_make_writable(dst), "av_frame_make_writable");
  const uint8_t* src[1] = {rgb};
  const int stride[1] = {3 * src_w_};
  sws_scale(sws_, src, stride, 0, src_h_, dst->data, dst->linesize);
}

}  // namespace uti120

#include "log.hpp"
#include "media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace uti120 {

struct VideoWriter::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* enc = nullptr;
  AVStream* stream = nullptr;
  AVPacket* pkt = nullptr;
  std::unique_ptr<Scaler> scaler;
  FramePtr frame;
  bool header_written = false;

  ~Impl() {
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    if (fmt && !(fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
  }
};

VideoWriter::VideoWriter(const std::string& path, int src_w, int src_h, int scale, int fps)
    : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  check_av(avformat_alloc_output_context2(&m.fmt, nullptr, "mp4", path.c_str()),
           "allocating MP4 output");
  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) throw MediaError("libx264 encoder not available");
  m.enc = avcodec_alloc_context3(codec);
  m.enc->width = src_w * scale;
  m.enc->height = src_h * scale;
  m.enc->pix_fmt = AV_PIX_FMT_YUV420P;
  m.enc->time_base = AVRational{1, fps};
  m.enc->framerate = AVRational{fps, 1};
  if (m.fmt->oformat->flags & AVFMT_GLOBALHEADER) m.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  av_opt_set(m.enc->priv_data, "crf", "18", 0);
  check_av(avcodec_open2(m.enc, codec, nullptr), "opening libx264");

  m.stream = avformat_new_stream(m.fmt, nullptr);
  if (!m.stream) throw MediaError("avformat_new_stream failed");
  m.stream->time_base = m.enc->time_base;
  check_av(avcodec_parameters_from_context(m.stream->codecpar, m.enc), "codec parameters");
  check_av(avio_open(&m.fmt->pb, path.c_str(), AVIO_FLAG_WRITE), ("opening " + path).c_str());
  check_av(avformat_write_header(m.fmt, nullptr), "writing MP4 header");
  m.header_written = true;

  m.pkt = av_packet_alloc();
  m.scaler = std::make_unique<Scaler>(src_w, src_h, m.enc->width, m.enc->height,
                                      AV_PIX_FMT_YUV420P);
  m.frame = m.scaler->alloc_frame();
}

VideoWriter::~VideoWriter() {
  if (!finished_) {
    try {
      finish();
    } catch (const std::exception& e) {
      LOG_WARNING("uti120.video", "finishing the video failed: %s", e.what());
    }
  }
}

void VideoWriter::write(const uint8_t* rgb) {
  Impl& m = *impl_;
  m.scaler->scale(rgb, m.frame.get());
  m.frame->pts = pts_++;
  check_av(avcodec_send_frame(m.enc, m.frame.get()), "encoding frame");
  drain(false);
}

void VideoWriter::drain(bool flush) {
  Impl& m = *impl_;
  for (;;) {
    int rc = avcodec_receive_packet(m.enc, m.pkt);
    if (rc == AVERROR(EAGAIN) || (flush && rc == AVERROR_EOF)) return;
    check_av(rc, "receiving packet");
    // Every frame lasts exactly one tick of the constant frame rate.  Without
    // an explicit duration the MP4 muxer cannot know the last frame's length
    // and the video comes out one frame short (measured: 9.96 s for 250 frames).
    m.pkt->duration = 1;
    av_packet_rescale_ts(m.pkt, m.enc->time_base, m.stream->time_base);
    m.pkt->stream_index = m.stream->index;
    check_av(av_interleaved_write_frame(m.fmt, m.pkt), "writing packet");
  }
}

void VideoWriter::finish() {
  finished_ = true;
  Impl& m = *impl_;
  check_av(avcodec_send_frame(m.enc, nullptr), "flushing encoder");
  drain(true);
  check_av(av_write_trailer(m.fmt), "writing MP4 trailer");
}

}  // namespace uti120

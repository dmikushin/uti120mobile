#include <cstdio>
#include <cstring>
#include <memory>

#include "media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace uti120 {

namespace {
struct FileCloser {
  void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File open_for_write(const std::string& path) {
  File f(std::fopen(path.c_str(), "wb"));
  if (!f) throw MediaError("cannot open " + path + ": " + std::strerror(errno));
  return f;
}
}  // namespace

void write_png(const std::string& path, const uint8_t* rgb, int w, int h, int scale) {
  Scaler scaler(w, h, w * scale, h * scale, AV_PIX_FMT_RGB24);
  FramePtr frame = scaler.alloc_frame();
  scaler.scale(rgb, frame.get());

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
  if (!codec) throw MediaError("PNG encoder not available");
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> enc(
      avcodec_alloc_context3(codec), [](AVCodecContext* c) { avcodec_free_context(&c); });
  enc->width = scaler.dst_w;
  enc->height = scaler.dst_h;
  enc->pix_fmt = AV_PIX_FMT_RGB24;
  enc->time_base = AVRational{1, 1};
  check_av(avcodec_open2(enc.get(), codec, nullptr), "opening PNG encoder");
  check_av(avcodec_send_frame(enc.get(), frame.get()), "encoding PNG");
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(),
                                                     [](AVPacket* p) { av_packet_free(&p); });
  check_av(avcodec_receive_packet(enc.get(), pkt.get()), "receiving PNG");

  File f = open_for_write(path);
  if (std::fwrite(pkt->data, 1, pkt->size, f.get()) != size_t(pkt->size))
    throw MediaError("cannot write " + path);
}

void write_npy(const std::string& path, const std::vector<float>& data, int rows, int cols) {
  std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
                       std::to_string(rows) + ", " + std::to_string(cols) + "), }";
  // Magic (6) + version (2) + header length (2) + header, padded to 64 with a newline.
  size_t total = 10 + header.size() + 1;
  header.append((64 - total % 64) % 64, ' ');
  header += '\n';
  uint16_t hlen = uint16_t(header.size());

  File f = open_for_write(path);
  const char magic[] = "\x93NUMPY\x01\x00";
  bool ok = std::fwrite(magic, 1, 8, f.get()) == 8 &&
            std::fwrite(&hlen, 2, 1, f.get()) == 1 &&
            std::fwrite(header.data(), 1, header.size(), f.get()) == header.size() &&
            std::fwrite(data.data(), sizeof(float), data.size(), f.get()) == data.size();
  if (!ok) throw MediaError("cannot write " + path);
}

}  // namespace uti120

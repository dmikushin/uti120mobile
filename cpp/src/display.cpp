#include <SDL3/SDL.h>

#include "media.hpp"

extern "C" {
#include <libavutil/frame.h>
}

namespace uti120 {

struct Display::Impl {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  std::unique_ptr<Scaler> scaler;
  FramePtr frame;
};

static MediaError sdl_error(const char* what) {
  return MediaError(std::string(what) + ": " + SDL_GetError());
}

Display::Display(const std::string& title, int src_w, int src_h, int scale)
    : impl_(std::make_unique<Impl>()) {
  if (!SDL_Init(SDL_INIT_VIDEO)) throw sdl_error("SDL_Init");
  Impl& m = *impl_;
  int w = src_w * scale, h = src_h * scale;
  if (!SDL_CreateWindowAndRenderer(title.c_str(), w, h, 0, &m.window, &m.renderer))
    throw sdl_error("creating window");
  m.texture = SDL_CreateTexture(m.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                w, h);
  if (!m.texture) throw sdl_error("creating texture");
  // Upscaling is done by swscale (bicubic, as in the recorded video), so the
  // texture is shown 1:1.
  m.scaler = std::make_unique<Scaler>(src_w, src_h, w, h, AV_PIX_FMT_RGB24);
  m.frame = m.scaler->alloc_frame();
}

Display::~Display() {
  if (impl_->texture) SDL_DestroyTexture(impl_->texture);
  if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
  if (impl_->window) SDL_DestroyWindow(impl_->window);
  SDL_Quit();
}

void Display::show(const uint8_t* rgb) {
  Impl& m = *impl_;
  m.scaler->scale(rgb, m.frame.get());
  if (!SDL_UpdateTexture(m.texture, nullptr, m.frame->data[0], m.frame->linesize[0]))
    throw sdl_error("updating texture");
  SDL_RenderClear(m.renderer);
  SDL_RenderTexture(m.renderer, m.texture, nullptr, nullptr);
  SDL_RenderPresent(m.renderer);
}

bool Display::poll() {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
    if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q))
      return false;
  }
  return true;
}

}  // namespace uti120

#include "uti120/process.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace uti120 {

double median(std::vector<double> v) {
  if (v.empty()) throw std::invalid_argument("median of empty set");
  size_t n = v.size(), mid = n / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  double hi = v[mid];
  if (n % 2) return hi;
  double lo = *std::max_element(v.begin(), v.begin() + mid);
  return (lo + hi) / 2;
}

double percentile(std::vector<double> v, double p) {
  // Linear interpolation between the order statistics at floor(pos) and
  // floor(pos) + 1, found by selection rather than a full sort.
  double pos = p / 100.0 * double(v.size() - 1);
  size_t i = size_t(std::floor(pos));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  double lo = v[i];
  if (i + 1 >= v.size()) return lo;
  double hi = *std::min_element(v.begin() + i + 1, v.end());
  return lo + (pos - double(i)) * (hi - lo);
}

Calibration::Calibration(Plane dark) : dark_(std::move(dark)), bad_(find_bad_pixels(dark_)) {}

Plane mean_pixels(const std::vector<Frame>& frames) {
  std::vector<double> sum(PIXELS, 0.0);
  for (const Frame& f : frames)
    for (int i = 0; i < PIXELS; ++i) sum[i] += f.pixels()[i];
  Plane mean(PIXELS);
  for (int i = 0; i < PIXELS; ++i) mean[i] = float(sum[i] / double(frames.size()));
  return mean;
}

Calibration Calibration::from_frames(const std::vector<Frame>& frames) {
  return Calibration(mean_pixels(frames));
}

int Calibration::bad_count() const { return int(std::count(bad_.begin(), bad_.end(), 1)); }

Plane Calibration::apply(const uint16_t* pixels) const {
  Plane img(PIXELS);
  Mask bad(PIXELS);
  bool any = false;
  for (int i = 0; i < PIXELS; ++i) {
    img[i] = float(pixels[i]) - dark_[i];
    bad[i] = bad_[i] || pixels[i] == 0 || pixels[i] == ADC_MAX;
    any |= bad[i] != 0;
  }
  return any ? replace_pixels(img, bad) : img;
}

Plane median3x3(const Plane& img) {
  Plane out(PIXELS);
  std::vector<double> win(9);
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      int n = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          int yy = std::clamp(y + dy, 0, HEIGHT - 1), xx = std::clamp(x + dx, 0, WIDTH - 1);
          win[n++] = img[yy * WIDTH + xx];
        }
      out[y * WIDTH + x] = float(median(win));
    }
  return out;
}

Mask find_bad_pixels(const Plane& dark, float k) {
  Plane local = median3x3(dark);
  std::vector<double> resid(PIXELS);
  for (int i = 0; i < PIXELS; ++i) resid[i] = double(dark[i] - local[i]);
  double m = median(resid);
  std::vector<double> dev(PIXELS);
  for (int i = 0; i < PIXELS; ++i) dev[i] = std::abs(resid[i] - m);
  double mad = median(dev) + 1e-6;
  Mask bad(PIXELS);
  for (int i = 0; i < PIXELS; ++i)
    bad[i] = std::abs(resid[i]) > k * 1.4826 * mad || dark[i] <= 0 || dark[i] >= ADC_MAX;
  return bad;
}

Plane replace_pixels(const Plane& img, const Mask& mask) {
  Plane out = img;
  std::optional<double> fallback;
  std::vector<double> win;
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) {
      if (!mask[y * WIDTH + x]) continue;
      win.clear();
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          int yy = y + dy, xx = x + dx;
          if (yy < 0 || yy >= HEIGHT || xx < 0 || xx >= WIDTH || mask[yy * WIDTH + xx]) continue;
          win.push_back(img[yy * WIDTH + xx]);
        }
      if (win.empty()) {
        if (!fallback) fallback = median(std::vector<double>(img.begin(), img.end()));
        out[y * WIDTH + x] = float(*fallback);
      } else {
        out[y * WIDTH + x] = float(median(win));
      }
    }
  return out;
}

double row_stripes(const Plane& img) {
  std::vector<double> rows(HEIGHT);
  for (int y = 0; y < HEIGHT; ++y)
    rows[y] = median(std::vector<double>(img.begin() + y * WIDTH, img.begin() + (y + 1) * WIDTH));
  std::vector<double> d2(HEIGHT - 2);
  for (int y = 0; y < HEIGHT - 2; ++y) d2[y] = std::abs(rows[y + 2] - 2 * rows[y + 1] + rows[y]);
  return median(d2);
}

std::vector<float> AutoGain::operator()(const Plane& img) {
  std::vector<double> v(img.begin(), img.end());
  double lo = percentile(v, low_), hi = percentile(v, high_);
  if (!range_) {
    range_ = {lo, hi};
  } else {
    range_->first = smoothing_ * range_->first + (1 - smoothing_) * lo;
    range_->second = smoothing_ * range_->second + (1 - smoothing_) * hi;
  }
  auto [rlo, rhi] = *range_;
  double span = std::max(rhi - rlo, 1.0);
  std::vector<float> out(img.size());
  for (size_t i = 0; i < img.size(); ++i)
    out[i] = float(std::clamp((double(img[i]) - rlo) / span, 0.0, 1.0));
  return out;
}

}  // namespace uti120

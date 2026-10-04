// Turning raw detector counts into a picture.
//
// The raw microbolometer image is dominated by per-pixel offsets (fixed pattern
// noise) that are far larger than the scene contrast.  The shutter is a uniform
// reference: the mean of a few closed-shutter frames is the per-pixel offset map,
// and subtracting it from open-shutter frames leaves the scene signal.
//
// The arithmetic follows the Python reference implementation (numpy semantics:
// the median of an even count is the mean of the two middle values, percentiles
// interpolate linearly).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "uti120/frame.hpp"

namespace uti120 {

constexpr uint16_t ADC_MAX = 0x3FFF;

// HEIGHT x WIDTH, row-major.
using Plane = std::vector<float>;
using Mask = std::vector<uint8_t>;

// Per-pixel mean of the frames' active images.
Plane mean_pixels(const std::vector<Frame>& frames);

class Calibration {
 public:
  explicit Calibration(Plane dark);
  static Calibration from_frames(const std::vector<Frame>& frames);

  // Offset-corrected signal (counts; warmer is larger).
  Plane apply(const uint16_t* pixels) const;

  const Plane& dark() const { return dark_; }
  const Mask& bad() const { return bad_; }
  int bad_count() const;

 private:
  Plane dark_;
  Mask bad_;
};

// Pixels whose closed-shutter level is saturated or far from their neighbours.
Mask find_bad_pixels(const Plane& dark, float k = 8.0f);
Plane median3x3(const Plane& img);
// Replace masked pixels by the median of their unmasked 3x3 neighbours.
Plane replace_pixels(const Plane& img, const Mask& mask);
// Strength of row-wise offsets: median |2nd difference| of row medians.  A
// calibrated frame of a real scene measures ~8-10 counts; frames taken while
// the camera's NUC is still settling measure 30-240.
double row_stripes(const Plane& img);

double median(std::vector<double> v);
double percentile(std::vector<double> v, double p);

// Maps the signal to [0, 1] between low/high percentiles, smoothed over time.
class AutoGain {
 public:
  AutoGain(double low = 1.0, double high = 99.0, double smoothing = 0.8)
      : low_(low), high_(high), smoothing_(smoothing) {}
  std::vector<float> operator()(const Plane& img);

 private:
  double low_, high_, smoothing_;
  std::optional<std::pair<double, double>> range_;
};

}  // namespace uti120

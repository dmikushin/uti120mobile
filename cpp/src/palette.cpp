#include "palette.hpp"

#include <cmath>
#include <stdexcept>

namespace uti120::palette {

namespace {

struct Stop {
  double pos;
  uint8_t rgb[3];
};

struct Named {
  const char* name;
  std::vector<Stop> stops;
};

const std::vector<Named>& table() {
  static const std::vector<Named> t = {
      {"grey", {{0.0, {0, 0, 0}}, {1.0, {255, 255, 255}}}},
      {"ironbow",
       {{0.0, {0, 0, 0}}, {0.15, {32, 0, 96}}, {0.35, {128, 0, 160}}, {0.55, {220, 40, 60}},
        {0.75, {250, 140, 0}}, {0.9, {255, 220, 40}}, {1.0, {255, 255, 230}}}},
      {"rainbow",
       {{0.0, {0, 0, 128}}, {0.2, {0, 0, 255}}, {0.4, {0, 255, 255}}, {0.6, {255, 255, 0}},
        {0.8, {255, 0, 0}}, {1.0, {128, 0, 0}}}},
  };
  return t;
}

}  // namespace

const char* const NAMES[3] = {"grey", "ironbow", "rainbow"};

Lut lut(const std::string& name) {
  for (const Named& n : table()) {
    if (name != n.name) continue;
    Lut out{};
    for (int i = 0; i < 256; ++i) {
      double x = i / 255.0;
      size_t s = 0;
      while (s + 2 < n.stops.size() && x > n.stops[s + 1].pos) ++s;
      const Stop &a = n.stops[s], &b = n.stops[s + 1];
      double t = (x - a.pos) / (b.pos - a.pos);
      for (int c = 0; c < 3; ++c)
        // nearbyint rounds half to even, like numpy's round().
        out[i][c] = uint8_t(std::nearbyint(a.rgb[c] + t * (b.rgb[c] - a.rgb[c])));
    }
    return out;
  }
  throw std::invalid_argument("unknown palette " + name);
}

std::vector<uint8_t> colorize(const std::vector<float>& norm, const Lut& lut) {
  std::vector<uint8_t> rgb(norm.size() * 3);
  for (size_t i = 0; i < norm.size(); ++i) {
    const auto& c = lut[int(std::nearbyint(double(norm[i]) * 255.0))];
    rgb[3 * i] = c[0];
    rgb[3 * i + 1] = c[1];
    rgb[3 * i + 2] = c[2];
  }
  return rgb;
}

}  // namespace uti120::palette

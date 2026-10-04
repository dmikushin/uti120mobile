// False-colour palettes as 256-entry RGB lookup tables.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace uti120::palette {

using Lut = std::array<std::array<uint8_t, 3>, 256>;

extern const char* const NAMES[3];

// Throws std::invalid_argument for an unknown name.
Lut lut(const std::string& name);

// norm in [0, 1] -> packed RGB24.
std::vector<uint8_t> colorize(const std::vector<float>& norm, const Lut& lut);

}  // namespace uti120::palette

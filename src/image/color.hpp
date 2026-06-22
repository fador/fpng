#pragma once

#include <cstdint>
#include <cstddef>

namespace fpng {

inline constexpr uint8_t alpha_max_for_depth(uint8_t bit_depth) noexcept {
    switch (bit_depth) {
    case 1:  return 1;
    case 2:  return 3;
    case 4:  return 15;
    case 8:  return 255;
    case 16: return 255; // max byte value, high byte
    default: return 0;
    }
}

inline constexpr uint8_t color_type_to_channels(uint8_t color_type) noexcept {
    switch (color_type) {
    case 0: return 1; // Grayscale
    case 2: return 3; // Truecolor
    case 3: return 1; // Indexed (palette index)
    case 4: return 2; // Grayscale + Alpha
    case 6: return 4; // Truecolor + Alpha
    default: return 0;
    }
}

inline constexpr bool color_type_has_alpha(uint8_t color_type) noexcept {
    return color_type == 4 || color_type == 6;
}

inline constexpr size_t bits_per_pixel(uint8_t color_type, uint8_t bit_depth) noexcept {
    return static_cast<size_t>(color_type_to_channels(color_type)) * bit_depth;
}

} // namespace fpng

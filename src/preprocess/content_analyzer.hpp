#pragma once
#include "image/image.hpp"
namespace fpng {
struct ImageContent {
    bool is_photographic = false;
    bool is_graphics = true;
    float gradient_strength = 0;
    size_t unique_colors = 0;
    float entropy_r = 0, entropy_g = 0, entropy_b = 0, entropy_a = 0;
};
ImageContent analyze_content(const Image& img);
} // namespace fpng

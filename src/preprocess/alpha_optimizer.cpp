#include "preprocess/alpha_optimizer.hpp"

#include <cstring>
#include <vector>

namespace fpng {

namespace {

// Number of bytes per sample (1 for 8-bit, 2 for 16-bit).
inline size_t sample_bytes(uint8_t bit_depth) {
    return bit_depth > 8 ? 2 : 1;
}

// True when a sample (of `s` bytes, big-endian) equals the maximum value.
inline bool sample_is_max(const uint8_t* p, size_t s) {
    for (size_t i = 0; i < s; ++i)
        if (p[i] != 0xff) return false;
    return true;
}

inline bool sample_is_zero(const uint8_t* p, size_t s) {
    for (size_t i = 0; i < s; ++i)
        if (p[i] != 0) return false;
    return true;
}

} // namespace

void alpha_optimize(Image& img) {
    size_t s = sample_bytes(img.bit_depth);

    if (img.color_type == 6) { // RGBA
        size_t bpp = 4 * s;
        size_t row_size = img.raw_scanline_size();
        // Zero RGB of fully-transparent pixels
        for (size_t y = 0; y < img.height; ++y) {
            uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                uint8_t* px = row + x * bpp;
                if (sample_is_zero(px + 3 * s, s))
                    std::memset(px, 0, 3 * s);
            }
        }

        // Strip alpha when every sample is fully opaque (RGBA -> RGB)
        bool all_opaque = true;
        for (size_t y = 0; y < img.height && all_opaque; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                if (!sample_is_max(row + x * bpp + 3 * s, s)) {
                    all_opaque = false;
                    break;
                }
            }
        }
        if (all_opaque) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.width * img.height * 3 * s);
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                for (size_t x = 0; x < img.width; ++x) {
                    const uint8_t* px = row + x * bpp;
                    new_pixels.insert(new_pixels.end(), px, px + 3 * s);
                }
            }
            img.pixels = std::move(new_pixels);
            img.color_type = 2; // RGB
        }
    } else if (img.color_type == 4) { // Grayscale + Alpha
        size_t bpp = 2 * s;
        size_t row_size = img.raw_scanline_size();
        for (size_t y = 0; y < img.height; ++y) {
            uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                uint8_t* px = row + x * bpp;
                if (sample_is_zero(px + s, s))
                    std::memset(px, 0, s);
            }
        }

        bool all_opaque = true;
        for (size_t y = 0; y < img.height && all_opaque; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                if (!sample_is_max(row + x * bpp + s, s)) {
                    all_opaque = false;
                    break;
                }
            }
        }
        if (all_opaque) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.width * img.height * s);
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                for (size_t x = 0; x < img.width; ++x)
                    new_pixels.insert(new_pixels.end(), row + x * bpp,
                                      row + x * bpp + s);
            }
            img.pixels = std::move(new_pixels);
            img.color_type = 0; // Grayscale
        }
    }
}

} // namespace fpng

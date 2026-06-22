#include "preprocess/alpha_optimizer.hpp"

#include <cstring>

namespace fpng {

void alpha_optimize(Image& img) {
    size_t bpp = img.bytes_per_pixel();
    size_t row_size = img.raw_scanline_size();

    if (img.color_type == 6) { // RGBA
        // Zero RGB of fully-transparent pixels
        for (size_t y = 0; y < img.height; ++y) {
            uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                size_t off = x * bpp;
                uint8_t a = row[off + 3];
                if (a == 0) {
                    row[off + 0] = 0;
                    row[off + 1] = 0;
                    row[off + 2] = 0;
                }
            }
        }
    } else if (img.color_type == 4) { // Grayscale + Alpha
        for (size_t y = 0; y < img.height; ++y) {
            uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                size_t off = x * bpp;
                if (row[off + 1] == 0)
                    row[off + 0] = 0;
            }
        }
    }

    // Check if alpha channel is all 255 -> reduce to non-alpha color type
    if (img.color_type == 6) {
        bool all_opaque = true;
        for (size_t y = 0; y < img.height && all_opaque; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                if (row[x * bpp + 3] != 255) {
                    all_opaque = false;
                    break;
                }
            }
        }
        if (all_opaque) {
            // Strip alpha channel
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.width * img.height * 3);
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                for (size_t x = 0; x < img.width; ++x) {
                    new_pixels.push_back(row[x * bpp + 0]);
                    new_pixels.push_back(row[x * bpp + 1]);
                    new_pixels.push_back(row[x * bpp + 2]);
                }
            }
            img.pixels = std::move(new_pixels);
            img.color_type = 2; // RGB
        }
    } else if (img.color_type == 4) {
        bool all_opaque = true;
        for (size_t y = 0; y < img.height && all_opaque; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                if (row[x * bpp + 1] != 255) {
                    all_opaque = false;
                    break;
                }
            }
        }
        if (all_opaque) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.width * img.height);
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                for (size_t x = 0; x < img.width; ++x)
                    new_pixels.push_back(row[x * bpp]);
            }
            img.pixels = std::move(new_pixels);
            img.color_type = 0; // Grayscale
        }
    }
}

} // namespace fpng

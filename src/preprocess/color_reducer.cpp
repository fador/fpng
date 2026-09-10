#include "preprocess/color_reducer.hpp"

#include <unordered_map>
#include <vector>
#include <cstdint>

namespace fpng {

void reduce_colors(Image& img) {
    // --- 16-bit -> 8-bit when the high byte of every sample is zero ---
    if (img.color_type != 3 && img.bit_depth == 16) {
        size_t n = img.pixels.size();
        bool can_reduce = (n % 2 == 0);
        for (size_t i = 0; i + 1 < n && can_reduce; i += 2)
            if (img.pixels[i + 1] != 0) can_reduce = false;
        if (can_reduce) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(n / 2);
            for (size_t i = 0; i < n; i += 2)
                new_pixels.push_back(img.pixels[i]);
            img.pixels = std::move(new_pixels);
            img.bit_depth = 8;
        }
    }

    // Sample-level reductions below assume one byte per sample.
    if (img.bit_depth != 8) return;

    // --- RGB/RGBA -> Grayscale / Grayscale+Alpha when R==G==B ---
    if (img.color_type == 2 || img.color_type == 6) {
        size_t bpp = img.bytes_per_pixel();
        size_t row_size = img.raw_scanline_size();
        bool all_gray = true;
        for (size_t y = 0; y < img.height && all_gray; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                const uint8_t* px = row + x * bpp;
                if (px[0] != px[1] || px[1] != px[2]) { all_gray = false; break; }
            }
        }
        if (all_gray) {
            std::vector<uint8_t> new_pixels;
            if (img.color_type == 2) {
                new_pixels.reserve(static_cast<size_t>(img.width) * img.height);
                for (size_t y = 0; y < img.height; ++y) {
                    const uint8_t* row = img.pixels.data() + y * row_size;
                    for (size_t x = 0; x < img.width; ++x)
                        new_pixels.push_back(row[x * bpp]);
                }
                img.pixels = std::move(new_pixels);
                img.color_type = 0;
            } else {
                new_pixels.reserve(static_cast<size_t>(img.width) * img.height * 2);
                for (size_t y = 0; y < img.height; ++y) {
                    const uint8_t* row = img.pixels.data() + y * row_size;
                    for (size_t x = 0; x < img.width; ++x) {
                        const uint8_t* px = row + x * bpp;
                        new_pixels.push_back(px[0]);
                        new_pixels.push_back(px[3]);
                    }
                }
                img.pixels = std::move(new_pixels);
                img.color_type = 4;
            }
        }
    }

    // --- Truecolor -> Indexed when there are at most 256 unique colors ---
    if (img.color_type == 2 || img.color_type == 6) {
        size_t bpp = img.bytes_per_pixel();
        size_t row_size = img.raw_scanline_size();
        bool has_alpha = (img.color_type == 6);

        std::unordered_map<uint32_t, uint8_t> index_of;
        index_of.reserve(512);
        std::vector<uint32_t> colors;   // packed RGBA
        bool too_many = false;

        for (size_t y = 0; y < img.height && !too_many; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                const uint8_t* px = row + x * bpp;
                uint32_t key = (uint32_t(px[0]) << 24) | (uint32_t(px[1]) << 16) |
                               (uint32_t(px[2]) << 8) | (has_alpha ? px[3] : 255u);
                if (index_of.find(key) == index_of.end()) {
                    if (colors.size() >= 256) { too_many = true; break; }
                    index_of.emplace(key, static_cast<uint8_t>(colors.size()));
                    colors.push_back(key);
                }
            }
        }

        if (!too_many) {
            size_t count = colors.size();
            img.palette.resize(count * 3);
            img.alpha_palette.clear();
            bool need_alpha = false;
            for (size_t i = 0; i < count; ++i) {
                uint32_t c = colors[i];
                uint8_t r = static_cast<uint8_t>((c >> 24) & 0xff);
                uint8_t g = static_cast<uint8_t>((c >> 16) & 0xff);
                uint8_t b = static_cast<uint8_t>((c >> 8) & 0xff);
                uint8_t a = static_cast<uint8_t>(c & 0xff);
                img.palette[i * 3 + 0] = r;
                img.palette[i * 3 + 1] = g;
                img.palette[i * 3 + 2] = b;
                if (a < 255) need_alpha = true;
                img.alpha_palette.push_back(a);
            }

            uint8_t bd = (count <= 2) ? 1 : (count <= 4) ? 2 : (count <= 16) ? 4 : 8;
            size_t row_bytes = (static_cast<size_t>(img.width) * bd + 7) / 8;
            std::vector<uint8_t> new_pixels(row_bytes * img.height, 0);
            int per = 8 / bd;
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                uint8_t* out_row = new_pixels.data() + y * row_bytes;
                for (size_t x = 0; x < img.width; ++x) {
                    const uint8_t* px = row + x * bpp;
                    uint32_t key = (uint32_t(px[0]) << 24) | (uint32_t(px[1]) << 16) |
                                   (uint32_t(px[2]) << 8) | (has_alpha ? px[3] : 255u);
                    uint8_t idx = index_of[key];
                    int slot = static_cast<int>(x % static_cast<size_t>(per));
                    int shift = 8 - bd * (slot + 1);
                    out_row[x / static_cast<size_t>(per)] |=
                        static_cast<uint8_t>(idx << shift);
                }
            }

            img.pixels = std::move(new_pixels);
            img.color_type = 3;
            img.bit_depth = bd;
            if (!need_alpha) img.alpha_palette.clear();
        }
    }
}

} // namespace fpng

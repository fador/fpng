#include "preprocess/color_reducer.hpp"

#include <unordered_map>
#include <vector>
#include <cstdint>

namespace fpng {

void reduce_colors(Image& img) {
    // --- 16-bit -> 8-bit when samples were scaled (high == low) or zero-extended (high == 0) ---
    if (img.color_type != 3 && img.bit_depth == 16) {
        size_t n = img.pixels.size();
        bool can_reduce_equal = (n % 2 == 0);
        bool can_reduce_zero_hi = (n % 2 == 0);
        for (size_t i = 0; i + 1 < n; i += 2) {
            if (img.pixels[i] != img.pixels[i + 1]) can_reduce_equal = false;
            if (img.pixels[i] != 0) can_reduce_zero_hi = false;
            if (!can_reduce_equal && !can_reduce_zero_hi) break;
        }
        if (can_reduce_equal) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(n / 2);
            for (size_t i = 0; i < n; i += 2)
                new_pixels.push_back(img.pixels[i]);
            img.pixels = std::move(new_pixels);
            img.bit_depth = 8;
        } else if (can_reduce_zero_hi) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(n / 2);
            for (size_t i = 0; i < n; i += 2)
                new_pixels.push_back(img.pixels[i + 1]);
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
            // Order palette: fully-transparent first, then semi-transparent,
            // then fully-opaque last. PNG allows a shorter tRNS (missing
            // trailing entries default to opaque 255), so placing opaque
            // entries last lets write_trns trim the trailing 255s.
            std::vector<uint32_t> ordered;
            ordered.reserve(count);
            for (size_t i = 0; i < count; ++i)
                if ((colors[i] & 0xffu) == 0u) ordered.push_back(colors[i]);
            for (size_t i = 0; i < count; ++i) {
                uint8_t a = static_cast<uint8_t>(colors[i] & 0xffu);
                if (a != 255 && a != 0) ordered.push_back(colors[i]);
            }
            for (size_t i = 0; i < count; ++i)
                if ((colors[i] & 0xffu) == 255u) ordered.push_back(colors[i]);

            // Build old->new index map and remap pixels below.
            std::vector<uint8_t> remap(count);
            for (size_t i = 0; i < count; ++i) {
                uint32_t key = colors[i];
                for (size_t j = 0; j < count; ++j) {
                    if (ordered[j] == key) { remap[i] = static_cast<uint8_t>(j); break; }
                }
            }

            img.palette.resize(count * 3);
            img.alpha_palette.clear();
            bool need_alpha = false;
            for (size_t i = 0; i < count; ++i) {
                uint32_t c = ordered[i];
                uint8_t r = static_cast<uint8_t>((c >> 24) & 0xff);
                uint8_t g = static_cast<uint8_t>((c >> 16) & 0xff);
                uint8_t b = static_cast<uint8_t>((c >> 8) & 0xff);
                uint8_t a = static_cast<uint8_t>(c & 0xff);
                // Zero RGB of fully-transparent entries (never displayed).
                if (a == 0) { r = g = b = 0; }
                img.palette[i * 3 + 0] = r;
                img.palette[i * 3 + 1] = g;
                img.palette[i * 3 + 2] = b;
                if (a < 255) need_alpha = true;
                img.alpha_palette.push_back(a);
            }

            // Remap the index_of lookup so pixel packing below uses new indices.
            for (auto& kv : index_of) {
                // kv.second is the old index; find its new index via remap.
                // We need the old key; rebuild from colors[old].
                (void)kv;
            }
            // Rebuild index_of with new indices (keys are unchanged).
            {
                std::unordered_map<uint32_t, uint8_t> new_index;
                new_index.reserve(index_of.size());
                for (size_t i = 0; i < count; ++i)
                    new_index.emplace(ordered[i], static_cast<uint8_t>(i));
                index_of = std::move(new_index);
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

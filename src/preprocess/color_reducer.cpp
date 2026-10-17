#include "preprocess/color_reducer.hpp"

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace fpng {

void reduce_colors(Image& img) {
    if (img.is_animated) return;

    // --- 16-bit -> 8-bit when samples were scaled (high == low) ---
    if (img.color_type != 3 && img.bit_depth == 16) {
        size_t n = img.pixels.size();
        bool can_reduce_equal = (n > 0 && n % 2 == 0);
        for (size_t i = 0; i + 1 < n; i += 2) {
            if (img.pixels[i] != img.pixels[i + 1]) {
                can_reduce_equal = false;
                break;
            }
        }
        // Check tRNS 16-bit samples if present
        if (can_reduce_equal && !img.ancillary.tRNS.empty()) {
            for (size_t i = 0; i + 1 < img.ancillary.tRNS.size(); i += 2) {
                if (img.ancillary.tRNS[i] != img.ancillary.tRNS[i + 1]) {
                    can_reduce_equal = false;
                    break;
                }
            }
        }
        // Skip if bKGD is present and not reducible
        if (can_reduce_equal && img.ancillary.bKGD) {
            const auto& bkgd = *img.ancillary.bKGD;
            for (size_t i = 0; i + 1 < bkgd.size(); i += 2) {
                if (bkgd[i] != bkgd[i + 1]) {
                    can_reduce_equal = false;
                    break;
                }
            }
        }
        if (can_reduce_equal) {
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(n / 2);
            for (size_t i = 0; i < n; i += 2)
                new_pixels.push_back(img.pixels[i]);
            img.pixels = std::move(new_pixels);
            img.bit_depth = 8;

            if (!img.ancillary.tRNS.empty()) {
                for (size_t i = 0; i + 1 < img.ancillary.tRNS.size(); i += 2)
                    img.ancillary.tRNS[i] = 0;
            }
            if (img.ancillary.bKGD) {
                auto& bkgd = *img.ancillary.bKGD;
                for (size_t i = 0; i + 1 < bkgd.size(); i += 2)
                    bkgd[i] = 0;
            }
        }
    }

    // --- Existing Indexed (color_type == 3): prune unused/duplicate palette entries & pack bit depth ---
    if (img.color_type == 3 && !img.palette.empty()) {
        size_t num_colors = img.palette.size() / 3;
        int bd = img.bit_depth;
        int per = 8 / bd;
        uint8_t mask = static_cast<uint8_t>((1u << bd) - 1);
        size_t row_bytes = img.raw_scanline_size();

        std::vector<uint32_t> freq(num_colors, 0);
        for (size_t y = 0; y < img.height; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_bytes;
            for (size_t bx = 0; bx < row_bytes; ++bx) {
                uint8_t byte = row[bx];
                for (int k = 0; k < per; ++k) {
                    size_t pixel = bx * static_cast<size_t>(per) + k;
                    if (pixel >= img.width) break;
                    int shift = 8 - bd * (k + 1);
                    uint8_t idx = static_cast<uint8_t>((byte >> shift) & mask);
                    if (idx < num_colors) freq[idx]++;
                }
            }
        }

        // Preserve bKGD palette index if present
        int bkgd_idx = -1;
        if (img.ancillary.bKGD && img.ancillary.bKGD->size() == 1) {
            uint8_t bi = (*img.ancillary.bKGD)[0];
            if (bi < num_colors) {
                bkgd_idx = bi;
                freq[bi]++;
            }
        }

        // Deduplicate used (R,G,B,A) colors
        std::unordered_map<uint32_t, size_t> unique_map;
        struct PalEntry { uint8_t r, g, b, a; };
        std::vector<PalEntry> unique_colors;
        std::vector<size_t> old_to_unique(num_colors, 0);

        for (size_t i = 0; i < num_colors; ++i) {
            if (freq[i] == 0) continue;
            uint8_t a = (i < img.alpha_palette.size()) ? img.alpha_palette[i] : 255;
            uint8_t r = (a == 0) ? 0 : img.palette[i * 3 + 0];
            uint8_t g = (a == 0) ? 0 : img.palette[i * 3 + 1];
            uint8_t b = (a == 0) ? 0 : img.palette[i * 3 + 2];
            uint32_t key = (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
            auto it = unique_map.find(key);
            if (it == unique_map.end()) {
                size_t uid = unique_colors.size();
                unique_map.emplace(key, uid);
                unique_colors.push_back({r, g, b, a});
                old_to_unique[i] = uid;
            } else {
                old_to_unique[i] = it->second;
            }
        }

        if (!unique_colors.empty() &&
            (unique_colors.size() < num_colors ||
             (unique_colors.size() <= 16 && img.bit_depth > 4) ||
             (unique_colors.size() <= 4 && img.bit_depth > 2) ||
             (unique_colors.size() <= 2 && img.bit_depth > 1))) {

            size_t count = unique_colors.size();
            // Order: transparent (a==0), semi-transparent (0<a<255), opaque (a==255)
            std::vector<size_t> order;
            order.reserve(count);
            for (size_t i = 0; i < count; ++i)
                if (unique_colors[i].a == 0) order.push_back(i);
            for (size_t i = 0; i < count; ++i)
                if (unique_colors[i].a > 0 && unique_colors[i].a < 255) order.push_back(i);
            for (size_t i = 0; i < count; ++i)
                if (unique_colors[i].a == 255) order.push_back(i);

            std::vector<uint8_t> unique_to_new(count, 0);
            for (size_t new_i = 0; new_i < count; ++new_i)
                unique_to_new[order[new_i]] = static_cast<uint8_t>(new_i);

            std::vector<uint8_t> remap(num_colors, 0);
            for (size_t i = 0; i < num_colors; ++i) {
                if (freq[i] > 0) remap[i] = unique_to_new[old_to_unique[i]];
            }

            img.palette.resize(count * 3);
            img.alpha_palette.clear();
            bool need_alpha = false;
            for (size_t new_i = 0; new_i < count; ++new_i) {
                const auto& ce = unique_colors[order[new_i]];
                img.palette[new_i * 3 + 0] = ce.r;
                img.palette[new_i * 3 + 1] = ce.g;
                img.palette[new_i * 3 + 2] = ce.b;
                if (ce.a < 255) need_alpha = true;
                img.alpha_palette.push_back(ce.a);
            }
            if (!need_alpha) {
                img.alpha_palette.clear();
            } else {
                while (!img.alpha_palette.empty() && img.alpha_palette.back() == 255)
                    img.alpha_palette.pop_back();
            }

            if (bkgd_idx >= 0) {
                (*img.ancillary.bKGD)[0] = remap[bkgd_idx];
            }

            uint8_t new_bd = (count <= 2) ? 1 : (count <= 4) ? 2 : (count <= 16) ? 4 : 8;
            if (new_bd > img.bit_depth) new_bd = img.bit_depth;

            size_t new_row_bytes = (static_cast<size_t>(img.width) * new_bd + 7) / 8;
            std::vector<uint8_t> new_pixels(new_row_bytes * img.height, 0);
            int new_per = 8 / new_bd;

            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_bytes;
                uint8_t* out_row = new_pixels.data() + y * new_row_bytes;
                for (size_t x = 0; x < img.width; ++x) {
                    size_t bx = x / static_cast<size_t>(per);
                    int k = static_cast<int>(x % static_cast<size_t>(per));
                    int shift = 8 - bd * (k + 1);
                    uint8_t old_idx = static_cast<uint8_t>((row[bx] >> shift) & mask);
                    uint8_t new_idx = (old_idx < num_colors) ? remap[old_idx] : 0;

                    int out_slot = static_cast<int>(x % static_cast<size_t>(new_per));
                    int out_shift = 8 - new_bd * (out_slot + 1);
                    out_row[x / static_cast<size_t>(new_per)] |=
                        static_cast<uint8_t>(new_idx << out_shift);
                }
            }

            img.pixels = std::move(new_pixels);
            img.bit_depth = new_bd;
        }
        return;
    }

    // Sample-level reductions below assume one byte per sample.
    if (img.bit_depth != 8) return;

    // --- RGB/RGBA -> Grayscale / Grayscale+Alpha when R==G==B ---
    if (img.color_type == 2 || img.color_type == 6) {
        size_t bpp = img.bytes_per_pixel();
        size_t row_size = img.raw_scanline_size();
        bool all_gray = true;

        // If RGB has a 6-byte tRNS color key, it can only reduce to Grayscale if
        // the color key itself is gray (R == G == B).
        if (img.color_type == 2 && !img.ancillary.tRNS.empty()) {
            if (img.ancillary.tRNS.size() != 6 ||
                img.ancillary.tRNS[1] != img.ancillary.tRNS[3] ||
                img.ancillary.tRNS[3] != img.ancillary.tRNS[5]) {
                all_gray = false;
            }
        }
        if (img.ancillary.bKGD) {
            const auto& bkgd = *img.ancillary.bKGD;
            if (bkgd.size() == 6 && (bkgd[1] != bkgd[3] || bkgd[3] != bkgd[5])) {
                all_gray = false;
            }
        }

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
                if (img.ancillary.tRNS.size() == 6) {
                    img.ancillary.tRNS = {img.ancillary.tRNS[0], img.ancillary.tRNS[1]};
                }
                if (img.ancillary.bKGD && img.ancillary.bKGD->size() == 6) {
                    *img.ancillary.bKGD = {(*img.ancillary.bKGD)[0], (*img.ancillary.bKGD)[1]};
                }
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
                if (img.ancillary.bKGD && img.ancillary.bKGD->size() == 6) {
                    *img.ancillary.bKGD = {(*img.ancillary.bKGD)[0], (*img.ancillary.bKGD)[1]};
                }
            }
        }
    }

    // --- 8-bit Grayscale (color_type == 0) -> 1/2/4-bit Grayscale when samples are exact multiples ---
    if (img.color_type == 0 && img.bit_depth == 8 &&
        img.ancillary.tRNS.empty() && !img.ancillary.bKGD && !img.ancillary.sBIT) {
        bool can_1bit = true;
        bool can_2bit = true;
        bool can_4bit = true;
        for (uint8_t v : img.pixels) {
            if (v != 0x00 && v != 0xff) can_1bit = false;
            if (v % 0x55 != 0) can_2bit = false;
            if (v % 0x11 != 0) { can_4bit = false; break; }
        }
        uint8_t new_bd = can_1bit ? 1 : (can_2bit ? 2 : (can_4bit ? 4 : 8));
        if (new_bd < 8) {
            size_t row_bytes = (static_cast<size_t>(img.width) * new_bd + 7) / 8;
            std::vector<uint8_t> new_pixels(row_bytes * img.height, 0);
            int per = 8 / new_bd;
            uint8_t divisor = (new_bd == 1) ? 0xff : (new_bd == 2) ? 0x55 : 0x11;
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * img.width;
                uint8_t* out_row = new_pixels.data() + y * row_bytes;
                for (size_t x = 0; x < img.width; ++x) {
                    uint8_t sample = row[x] / divisor;
                    int slot = static_cast<int>(x % static_cast<size_t>(per));
                    int shift = 8 - new_bd * (slot + 1);
                    out_row[x / static_cast<size_t>(per)] |=
                        static_cast<uint8_t>(sample << shift);
                }
            }
            img.pixels = std::move(new_pixels);
            img.bit_depth = new_bd;
            return;
        }
    }

    // --- Truecolor -> Indexed when there are at most 256 unique colors ---
    if ((img.color_type == 2 || img.color_type == 6) && !img.ancillary.bKGD && !img.ancillary.sBIT) {
        size_t bpp = img.bytes_per_pixel();
        size_t row_size = img.raw_scanline_size();
        bool has_alpha = (img.color_type == 6);
        bool has_rgb_trns = (img.color_type == 2 && img.ancillary.tRNS.size() == 6 &&
                             img.ancillary.tRNS[0] == 0 && img.ancillary.tRNS[2] == 0 &&
                             img.ancillary.tRNS[4] == 0);
        uint8_t trns_r = has_rgb_trns ? img.ancillary.tRNS[1] : 0;
        uint8_t trns_g = has_rgb_trns ? img.ancillary.tRNS[3] : 0;
        uint8_t trns_b = has_rgb_trns ? img.ancillary.tRNS[5] : 0;

        auto pack_pixel = [&](const uint8_t* px) -> uint32_t {
            uint8_t r = px[0], g = px[1], b = px[2];
            uint8_t a = has_alpha ? px[3] : 255u;
            if (has_rgb_trns && r == trns_r && g == trns_g && b == trns_b) {
                a = 0;
            }
            if (a == 0) { r = g = b = 0; }
            return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
        };

        std::unordered_map<uint32_t, uint8_t> index_of;
        index_of.reserve(512);
        std::vector<uint32_t> colors;   // packed RGBA
        bool too_many = false;

        for (size_t y = 0; y < img.height && !too_many; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                const uint8_t* px = row + x * bpp;
                uint32_t key = pack_pixel(px);
                if (index_of.find(key) == index_of.end()) {
                    if (colors.size() >= 256) { too_many = true; break; }
                    index_of.emplace(key, static_cast<uint8_t>(colors.size()));
                    colors.push_back(key);
                }
            }
        }

        if (!too_many && !colors.empty()) {
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

            img.palette.resize(count * 3);
            img.alpha_palette.clear();
            bool need_alpha = false;
            for (size_t i = 0; i < count; ++i) {
                uint32_t c = ordered[i];
                uint8_t r = static_cast<uint8_t>((c >> 24) & 0xff);
                uint8_t g = static_cast<uint8_t>((c >> 16) & 0xff);
                uint8_t b = static_cast<uint8_t>((c >> 8) & 0xff);
                uint8_t a = static_cast<uint8_t>(c & 0xff);
                if (a == 0) { r = g = b = 0; }
                img.palette[i * 3 + 0] = r;
                img.palette[i * 3 + 1] = g;
                img.palette[i * 3 + 2] = b;
                if (a < 255) need_alpha = true;
                img.alpha_palette.push_back(a);
            }

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
                    uint32_t key = pack_pixel(px);
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
            img.ancillary.tRNS.clear();
            if (!need_alpha) img.alpha_palette.clear();
        }
    }
}

} // namespace fpng

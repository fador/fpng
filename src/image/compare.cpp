#include "image/compare.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace fpng {

namespace {

using RGBA16 = std::array<uint16_t, 4>;

uint16_t scale16(uint32_t v, unsigned bd) {
    if (bd == 16) return static_cast<uint16_t>(v);
    uint32_t maxv = (1u << bd) - 1;
    return static_cast<uint16_t>(v * 65535u / maxv);
}

// Expand one row (of `w` pixels starting at `row`) into canonical RGBA16.
void expand_row(const Image& img, const uint8_t* row, uint32_t w, std::vector<RGBA16>& out) {
    out.resize(w);
    const unsigned bd = img.bit_depth;
    auto sample = [&](size_t idx) -> uint32_t {  // idx-th sample in the row
        if (bd == 16) return (uint32_t(row[idx * 2]) << 8) | row[idx * 2 + 1];
        if (bd == 8) return row[idx];
        size_t bit = idx * bd;
        return (row[bit / 8] >> (8 - bd - bit % 8)) & ((1u << bd) - 1);
    };
    const auto& t = img.ancillary.tRNS;
    for (uint32_t x = 0; x < w; ++x) {
        RGBA16 px{};
        switch (img.color_type) {
        case 0: {
            uint32_t g = sample(x);
            uint16_t s = scale16(g, bd);
            bool key = t.size() == 2 && g == ((uint32_t(t[0]) << 8) | t[1]);
            px = {s, s, s, static_cast<uint16_t>(key ? 0 : 65535)};
            break;
        }
        case 2: {
            uint32_t r = sample(x * 3), g = sample(x * 3 + 1), b = sample(x * 3 + 2);
            bool key = t.size() == 6 && r == ((uint32_t(t[0]) << 8) | t[1]) &&
                       g == ((uint32_t(t[2]) << 8) | t[3]) && b == ((uint32_t(t[4]) << 8) | t[5]);
            px = {scale16(r, bd), scale16(g, bd), scale16(b, bd), static_cast<uint16_t>(key ? 0 : 65535)};
            break;
        }
        case 3: {
            uint32_t i = sample(x);
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (i * 3 + 2 < img.palette.size()) {
                r = img.palette[i * 3]; g = img.palette[i * 3 + 1]; b = img.palette[i * 3 + 2];
            }
            if (i < img.alpha_palette.size()) a = img.alpha_palette[i];
            px = {scale16(r, 8), scale16(g, 8), scale16(b, 8), scale16(a, 8)};
            break;
        }
        case 4: {
            uint16_t g = scale16(sample(x * 2), bd);
            px = {g, g, g, scale16(sample(x * 2 + 1), bd)};
            break;
        }
        default:
            px = {scale16(sample(x * 4), bd), scale16(sample(x * 4 + 1), bd),
                  scale16(sample(x * 4 + 2), bd), scale16(sample(x * 4 + 3), bd)};
            break;
        }
        if (px[3] == 0) px = {0, 0, 0, 0};
        out[x] = px;
    }
}

bool compare_planes(const Image& a, const uint8_t* pa, const Image& b, const uint8_t* pb,
                    uint32_t w, uint32_t h, std::string* why, const char* what) {
    size_t sa = a.scanline_size_for_width(w), sb = b.scanline_size_for_width(w);
    std::vector<RGBA16> ra, rb;
    for (uint32_t y = 0; y < h; ++y) {
        expand_row(a, pa + y * sa, w, ra);
        expand_row(b, pb + y * sb, w, rb);
        for (uint32_t x = 0; x < w; ++x) {
            if (ra[x] != rb[x]) {
                if (why) *why = std::string(what) + ": pixel (" + std::to_string(x) + "," +
                                std::to_string(y) + ") differs";
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool images_equivalent(const Image& a, const Image& b, std::string* why) {
    if (a.width != b.width || a.height != b.height) {
        if (why) *why = "dimensions differ";
        return false;
    }
    if (a.pixels.size() < a.total_raw_size() || b.pixels.size() < b.total_raw_size()) {
        if (why) *why = "pixel buffer too small";
        return false;
    }
    if (!compare_planes(a, a.pixels.data(), b, b.pixels.data(), a.width, a.height, why, "image"))
        return false;

    bool anim_a = a.is_animated && !a.frames.empty();
    bool anim_b = b.is_animated && !b.frames.empty();
    if (anim_a != anim_b || (anim_a && (a.frames.size() != b.frames.size() || a.num_plays != b.num_plays))) {
        if (why) *why = "animation structure differs";
        return false;
    }
    if (!anim_a) return true;
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const auto& fa = a.frames[i];
        const auto& fb = b.frames[i];
        if (fa.width != fb.width || fa.height != fb.height || fa.x_offset != fb.x_offset ||
            fa.y_offset != fb.y_offset || fa.delay_num != fb.delay_num ||
            fa.delay_den != fb.delay_den || fa.dispose_op != fb.dispose_op ||
            fa.blend_op != fb.blend_op || fa.from_idat != fb.from_idat) {
            if (why) *why = "frame " + std::to_string(i) + " control data differs";
            return false;
        }
        if (fa.from_idat) continue;
        if (fa.image_data.size() < a.scanline_size_for_width(fa.width) * fa.height ||
            fb.image_data.size() < b.scanline_size_for_width(fb.width) * fb.height) {
            if (why) *why = "frame " + std::to_string(i) + " data too small";
            return false;
        }
        std::string what = "frame " + std::to_string(i);
        if (!compare_planes(a, fa.image_data.data(), b, fb.image_data.data(), fa.width, fa.height,
                            why, what.c_str()))
            return false;
    }
    return true;
}

} // namespace fpng

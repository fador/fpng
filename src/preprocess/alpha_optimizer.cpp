#include "preprocess/alpha_optimizer.hpp"

#include <cstring>
#include <vector>
#include <unordered_set>

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

// Read a big-endian sample of `s` bytes as a 16-bit value.
inline uint16_t sample_value(const uint8_t* p, size_t s) {
    return s == 2 ? static_cast<uint16_t>((p[0] << 8) | p[1]) : p[0];
}

// Append a 16-bit big-endian value to a byte vector.
inline void push_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xff));
}

// Try to convert RGBA (type 6) to RGB (type 2) + single-color-key tRNS.
// Requires strictly binary alpha (0 or max). After zeroing, all transparent
// pixels are (0,0,0); if no opaque pixel uses (0,0,0) we can drop the alpha
// channel and declare (0,0,0) transparent. Otherwise we search for an unused
// RGB value and recolor transparent pixels to it.
bool try_color_key_rgba(Image& img, size_t s) {
    size_t bpp = 4 * s;
    size_t row_size = img.raw_scanline_size();

    // 1. Verify strictly binary alpha and count transparent pixels.
    bool binary = true;
    bool has_transparent = false;
    for (size_t y = 0; y < img.height && binary; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + 3 * s, s)) { has_transparent = true; }
            else if (!sample_is_max(px + 3 * s, s)) { binary = false; break; }
        }
    }
    if (!binary || !has_transparent) return false;

    // 2. Check if any opaque pixel uses (0,0,0). After zeroing, transparent
    //    pixels are (0,0,0), so an opaque (0,0,0) would collide with the key.
    bool zero_used = false;
    for (size_t y = 0; y < img.height && !zero_used; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + 3 * s, s)) continue;
            if (sample_is_zero(px, 3 * s)) { zero_used = true; break; }
        }
    }

    // 3. Pick a color key not used by any opaque pixel.
    uint16_t key_r = 0, key_g = 0, key_b = 0;
    bool found = false;
    if (!zero_used) {
        found = true; // (0,0,0) is free
    } else if (s == 1) {
        // Search for an unused 8-bit RGB value.
        std::unordered_set<uint32_t> opaque_rgb;
        opaque_rgb.reserve(static_cast<size_t>(img.width) * img.height + 1);
        for (size_t y = 0; y < img.height; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                const uint8_t* px = row + x * bpp;
                if (sample_is_zero(px + 3, 1)) continue;
                uint32_t key = (uint32_t(px[0]) << 16) | (uint32_t(px[1]) << 8) | px[2];
                opaque_rgb.insert(key);
            }
        }
        for (uint32_t cand = 1; cand < 0x1000000u; ++cand) {
            if (opaque_rgb.find(cand) == opaque_rgb.end()) {
                key_r = static_cast<uint16_t>((cand >> 16) & 0xff);
                key_g = static_cast<uint16_t>((cand >> 8) & 0xff);
                key_b = static_cast<uint16_t>(cand & 0xff);
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    // 4. Recolor transparent pixels to the key and drop the alpha channel.
    std::vector<uint8_t> new_pixels;
    new_pixels.reserve(static_cast<size_t>(img.width) * img.height * 3 * s);
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + 3 * s, s)) {
                if (s == 2) {
                    new_pixels.push_back(static_cast<uint8_t>(key_r >> 8));
                    new_pixels.push_back(static_cast<uint8_t>(key_r & 0xff));
                    new_pixels.push_back(static_cast<uint8_t>(key_g >> 8));
                    new_pixels.push_back(static_cast<uint8_t>(key_g & 0xff));
                    new_pixels.push_back(static_cast<uint8_t>(key_b >> 8));
                    new_pixels.push_back(static_cast<uint8_t>(key_b & 0xff));
                } else {
                    new_pixels.push_back(static_cast<uint8_t>(key_r));
                    new_pixels.push_back(static_cast<uint8_t>(key_g));
                    new_pixels.push_back(static_cast<uint8_t>(key_b));
                }
            } else {
                new_pixels.insert(new_pixels.end(), px, px + 3 * s);
            }
        }
    }
    img.pixels = std::move(new_pixels);
    img.color_type = 2; // RGB

    // tRNS chunk: 6 bytes = R,G,B as 16-bit BE.
    img.ancillary.tRNS.clear();
    push_be16(img.ancillary.tRNS, key_r);
    push_be16(img.ancillary.tRNS, key_g);
    push_be16(img.ancillary.tRNS, key_b);
    return true;
}

// Try to convert Grayscale+Alpha (type 4) to Grayscale (type 0) + tRNS.
bool try_color_key_ga(Image& img, size_t s) {
    size_t bpp = 2 * s;
    size_t row_size = img.raw_scanline_size();

    bool binary = true;
    bool has_transparent = false;
    for (size_t y = 0; y < img.height && binary; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + s, s)) { has_transparent = true; }
            else if (!sample_is_max(px + s, s)) { binary = false; break; }
        }
    }
    if (!binary || !has_transparent) return false;

    std::unordered_set<uint16_t> opaque_gray;
    opaque_gray.reserve(static_cast<size_t>(img.width) * img.height + 1);
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + s, s)) continue;
            opaque_gray.insert(sample_value(px, s));
        }
    }

    // Pick a gray key not used by any opaque pixel.
    uint16_t key_g = 0;
    bool found = false;
    uint32_t max_val = (s == 2) ? 0xffffu : 0xffu;
    for (uint32_t cand = 0; cand <= max_val; ++cand) {
        if (opaque_gray.find(static_cast<uint16_t>(cand)) == opaque_gray.end()) {
            key_g = static_cast<uint16_t>(cand);
            found = true;
            break;
        }
    }
    if (!found) return false;

    std::vector<uint8_t> new_pixels;
    new_pixels.reserve(static_cast<size_t>(img.width) * img.height * s);
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        for (size_t x = 0; x < img.width; ++x) {
            const uint8_t* px = row + x * bpp;
            if (sample_is_zero(px + s, s)) {
                if (s == 2) {
                    new_pixels.push_back(static_cast<uint8_t>(key_g >> 8));
                    new_pixels.push_back(static_cast<uint8_t>(key_g & 0xff));
                } else {
                    new_pixels.push_back(static_cast<uint8_t>(key_g));
                }
            } else {
                new_pixels.insert(new_pixels.end(), px, px + s);
            }
        }
    }
    img.pixels = std::move(new_pixels);
    img.color_type = 0; // Grayscale

    img.ancillary.tRNS.clear();
    push_be16(img.ancillary.tRNS, key_g);
    return true;
}

} // namespace

void alpha_optimize(Image& img) {
    size_t s = sample_bytes(img.bit_depth);

    if (img.color_type == 3) {
        // Zero the RGB of fully-transparent palette entries. The RGB values of
        // alpha==0 entries are never displayed, so forcing them to (0,0,0)
        // is lossless and makes PLTE bytes more repetitive (tiny win) while
        // also making tRNS trailing-trim more likely after palette sorts.
        for (size_t i = 0; i < img.alpha_palette.size(); ++i) {
            if (img.alpha_palette[i] == 0 && (i * 3 + 2) < img.palette.size()) {
                img.palette[i * 3 + 0] = 0;
                img.palette[i * 3 + 1] = 0;
                img.palette[i * 3 + 2] = 0;
            }
        }
    } else if (img.color_type == 6) { // RGBA
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
        } else {
            // Binary alpha: try RGBA -> RGB + single-color-key tRNS.
            try_color_key_rgba(img, s);
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
        } else {
            // Binary alpha: try GA -> G + single-color-key tRNS.
            try_color_key_ga(img, s);
        }
    }

    // tRNS trailing-trim is applied at write time (PNGWriter::write_trns).
}

} // namespace fpng

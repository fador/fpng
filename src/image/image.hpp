#pragma once

#include "png/ihdr.hpp"

#include <vector>
#include <cstdint>
#include <cstddef>
#include <span>
#include <optional>
#include <string>

namespace fpng {

struct FrameInfo {
    uint32_t sequence_number = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t x_offset = 0;
    uint32_t y_offset = 0;
    uint16_t delay_num = 0;
    uint16_t delay_den = 0;
    uint8_t dispose_op = 0; // 0=none, 1=background, 2=previous
    uint8_t blend_op = 0;   // 0=source, 1=over

    std::vector<uint8_t> image_data;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t  bit_depth = 8;
    uint8_t  color_type = 6; // TruecolorAlpha
    bool     interlaced = false;

    std::vector<uint8_t> pixels;
    std::vector<uint8_t> palette;   // PLTE: 3 bytes per entry
    std::vector<uint8_t> alpha_palette; // tRNS alpha values for indexed

    // APNG support
    bool is_animated = false;
    uint32_t num_plays = 0; // 0 = infinite
    std::vector<FrameInfo> frames;

    // Ancillary chunks to preserve
    struct Ancillary {
        std::optional<std::vector<uint8_t>> gAMA;
        std::optional<std::vector<uint8_t>> cHRM;
        std::optional<std::vector<uint8_t>> sRGB;
        std::optional<std::vector<uint8_t>> iCCP;
        std::optional<std::vector<uint8_t>> sBIT;
        std::optional<std::vector<uint8_t>> bKGD;
        std::optional<std::vector<uint8_t>> pHYs;
        std::optional<std::vector<uint8_t>> tIME;
        std::optional<std::vector<uint8_t>> cICP;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> text_chunks;
        std::vector<uint8_t> tRNS; // for non-indexed
    } ancillary;

    bool valid() const noexcept;
    size_t pixel_count() const noexcept { return static_cast<size_t>(width) * height; }
    size_t raw_scanline_size() const noexcept;
    // Byte width of one scanline of `w` pixels, honoring bit-packed formats
    // (indexed and grayscale when bit_depth < 8).
    size_t scanline_size_for_width(uint32_t w) const noexcept;
    size_t bytes_per_pixel() const noexcept;
    size_t total_raw_size() const noexcept;
};

inline bool Image::valid() const noexcept {
    if (width == 0 || height == 0) return false;
    if (pixels.empty()) return false;

    size_t expected = total_raw_size();
    if (pixels.size() != expected) return false;

    if (color_type == 3) {
        if (palette.empty() || palette.size() % 3 != 0 || palette.size() > 768) return false;
        if (bit_depth > 8) return false;
    }

    return true;
}

inline size_t Image::raw_scanline_size() const noexcept {
    return scanline_size_for_width(width);
}

inline size_t Image::scanline_size_for_width(uint32_t w) const noexcept {
    if (color_type == 3) {
        return (static_cast<size_t>(w) * bit_depth + 7) / 8;
    }
    size_t ch = (color_type == 0 ? 1 : (color_type == 2 ? 3 : (color_type == 4 ? 2 : 4)));
    return (static_cast<size_t>(w) * ch * bit_depth + 7) / 8;
}

inline size_t Image::bytes_per_pixel() const noexcept {
    if (color_type == 3) return 1;
    size_t ch = (color_type == 0 ? 1 : (color_type == 2 ? 3 : (color_type == 4 ? 2 : 4)));
    return (ch * bit_depth + 7) / 8;
}

inline size_t Image::total_raw_size() const noexcept {
    return raw_scanline_size() * height;
}

} // namespace fpng

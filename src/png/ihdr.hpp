#pragma once

#include <cstdint>

namespace fpng {

enum class ColorType : uint8_t {
    Grayscale       = 0,
    Truecolor       = 2,
    IndexedColor    = 3,
    GrayscaleAlpha  = 4,
    TruecolorAlpha  = 6,
};

enum class InterlaceMethod : uint8_t {
    None  = 0,
    Adam7 = 1,
};

enum class CompressionMethod : uint8_t {
    Deflate = 0,
};

enum class FilterMethod : uint8_t {
    Adaptive = 0,
};

struct IHDRData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t  bit_depth = 8;
    ColorType color_type = ColorType::TruecolorAlpha;
    CompressionMethod compression_method = CompressionMethod::Deflate;
    FilterMethod filter_method = FilterMethod::Adaptive;
    InterlaceMethod interlace_method = InterlaceMethod::None;

    bool valid() const noexcept;
    uint8_t bytes_per_pixel() const noexcept;
    uint8_t samples_per_pixel() const noexcept;
    uint32_t bytes_per_scanline() const noexcept;
    uint32_t raw_scanline_size() const noexcept;
};

inline bool IHDRData::valid() const noexcept {
    if (width == 0 || height == 0) return false;

    switch (color_type) {
    case ColorType::Grayscale:
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
               bit_depth == 8 || bit_depth == 16;
    case ColorType::Truecolor:
        return bit_depth == 8 || bit_depth == 16;
    case ColorType::IndexedColor:
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
    case ColorType::GrayscaleAlpha:
        return bit_depth == 8 || bit_depth == 16;
    case ColorType::TruecolorAlpha:
        return bit_depth == 8 || bit_depth == 16;
    }
    return false;
}

inline uint8_t IHDRData::samples_per_pixel() const noexcept {
    switch (color_type) {
    case ColorType::Grayscale:      return 1;
    case ColorType::Truecolor:      return 3;
    case ColorType::IndexedColor:   return 1;
    case ColorType::GrayscaleAlpha: return 2;
    case ColorType::TruecolorAlpha: return 4;
    }
    return 0;
}

inline uint8_t IHDRData::bytes_per_pixel() const noexcept {
    if (color_type == ColorType::IndexedColor) return 1;
    return (samples_per_pixel() * bit_depth + 7) / 8;
}

inline uint32_t IHDRData::raw_scanline_size() const noexcept {
    if (color_type == ColorType::IndexedColor) {
        return (static_cast<uint32_t>(width) * bit_depth + 7) / 8;
    }
    return (static_cast<uint32_t>(width) * samples_per_pixel() * bit_depth + 7) / 8;
}

inline uint32_t IHDRData::bytes_per_scanline() const noexcept {
    return raw_scanline_size() + 1; // +1 for filter type byte
}

} // namespace fpng

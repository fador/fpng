#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <cstdlib>

namespace fpng {

enum class FilterType : uint8_t {
    None    = 0,
    Sub     = 1,
    Up      = 2,
    Average = 3,
    Paeth   = 4,
};

inline uint8_t paeth_predictor(int a, int b, int c) noexcept {
    int p  = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc)             return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

// Encode: src -> dst (dst must be byte_width+1 bytes, first byte = filter type)
void filter_scanline(FilterType type,
                     const uint8_t* src, uint8_t* dst,
                     size_t pixel_stride, size_t byte_width,
                     const uint8_t* prev_scanline);

// Decode: filtered -> recon (both byte_width bytes)
// prev_scanline is the already-reconstructed prior scanline (or null for first row)
void unfilter_scanline(FilterType type,
                       const uint8_t* filtered, uint8_t* recon,
                       size_t pixel_stride, size_t byte_width,
                       const uint8_t* prev_scanline);

} // namespace fpng

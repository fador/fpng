#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

// SIMD includes must be at file scope. Guard on the FPNG_HAS_* macros set by
// the build system (which also enable the matching codegen flags), not on
// compiler predefined macros that MSVC never sets.
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(FPNG_HAS_NEON)
#include <arm_neon.h>
#endif
#if defined(FPNG_HAS_AVX2)
#include <immintrin.h>
#endif
#if defined(FPNG_HAS_SSE4_2) && !defined(FPNG_HAS_AVX2)
#include <smmintrin.h>
#endif

namespace fpng {

struct LZMatch {
    uint16_t length = 0;
    uint16_t distance = 0;
};

// Portable count-trailing-zeros (input must be non-zero).
inline int ctz32(uint32_t x) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward(&idx, x);
    return static_cast<int>(idx);
#else
    return __builtin_ctz(x);
#endif
}

inline int ctz64(uint64_t x) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return static_cast<int>(idx);
#else
    return __builtin_ctzll(x);
#endif
}

namespace simd {

inline size_t match_length(const uint8_t* a, const uint8_t* b, size_t max_len) noexcept {
    size_t i = 0;

#if defined(FPNG_HAS_NEON)
    while (i + 16 <= max_len) {
        uint8x16_t va = vld1q_u8(a + i);
        uint8x16_t vb = vld1q_u8(b + i);
        uint8x16_t vcmp = vceqq_u8(va, vb);
        uint64x2_t vcmp64 = vreinterpretq_u64_u8(vcmp);
        uint64_t low = vgetq_lane_u64(vcmp64, 0);
        uint64_t high = vgetq_lane_u64(vcmp64, 1);
        
        if (low != 0xffffffffffffffffull) {
            i += ctz64(~low) / 8;
            return std::min(i, max_len);
        }
        if (high != 0xffffffffffffffffull) {
            i += 8 + ctz64(~high) / 8;
            return std::min(i, max_len);
        }
        i += 16;
    }
#elif defined(FPNG_HAS_AVX2)
    while (i + 32 <= max_len) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i vcmp = _mm256_cmpeq_epi8(va, vb);
        int mask = _mm256_movemask_epi8(vcmp);
        if (mask != -1) {
            i += ctz32(~static_cast<uint32_t>(mask));
            return std::min(i, max_len);
        }
        i += 32;
    }
#elif defined(FPNG_HAS_SSE4_2)
    while (i + 16 <= max_len) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i vcmp = _mm_cmpeq_epi8(va, vb);
        int mask = _mm_movemask_epi8(vcmp);
        if (mask != 0xffff) {
            i += ctz32(~static_cast<uint32_t>(mask) & 0xffff);
            return std::min(i, max_len);
        }
        i += 16;
    }
#endif

    while (i < max_len && a[i] == b[i]) ++i;
    return i;
}

} // namespace simd
} // namespace fpng

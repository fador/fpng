#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

// Platform-specific SIMD includes must be at file scope
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__SSE4_2__)
#include <smmintrin.h>
#endif

namespace fpng {
namespace simd {

inline size_t match_length(const uint8_t* a, const uint8_t* b, size_t max_len) noexcept {
    size_t i = 0;

#if defined(__ARM_NEON)
    while (i + 16 <= max_len) {
        uint8x16_t va = vld1q_u8(a + i);
        uint8x16_t vb = vld1q_u8(b + i);
        uint8x16_t vcmp = vceqq_u8(va, vb);
        uint64x2_t vcmp64 = vreinterpretq_u64_u8(vcmp);
        uint64_t low = vgetq_lane_u64(vcmp64, 0);
        uint64_t high = vgetq_lane_u64(vcmp64, 1);
        
        if (low != 0xffffffffffffffffull) {
            i += __builtin_ctzll(~low) / 8;
            return std::min(i, max_len);
        }
        if (high != 0xffffffffffffffffull) {
            i += 8 + __builtin_ctzll(~high) / 8;
            return std::min(i, max_len);
        }
        i += 16;
    }
#elif defined(__AVX2__)
    while (i + 32 <= max_len) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i vcmp = _mm256_cmpeq_epi8(va, vb);
        int mask = _mm256_movemask_epi8(vcmp);
        if (mask != -1) {
            i += __builtin_ctz(~mask);
            return std::min(i, max_len);
        }
        i += 32;
    }
#elif defined(__SSE4_2__)
    while (i + 16 <= max_len) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i vcmp = _mm_cmpeq_epi8(va, vb);
        int mask = _mm_movemask_epi8(vcmp);
        if (mask != 0xffff) {
            i += __builtin_ctz(~mask);
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

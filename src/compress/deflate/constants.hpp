#pragma once

#include <cstdint>
#include <cstddef>

namespace fpng {

// DEFLATE constants (RFC 1951)
namespace deflate {

inline constexpr int MAX_MATCH_LEN = 258;
inline constexpr int MIN_MATCH_LEN = 3;
inline constexpr int MAX_DIST = 32768;
inline constexpr int WINDOW_SIZE = 32768;

inline constexpr int MAX_BITS = 15;
inline constexpr int MAX_LITLEN_SYMS = 288;
inline constexpr int MAX_DIST_SYMS = 32;
inline constexpr int MAX_CLEN_SYMS = 19;

inline constexpr int END_OF_BLOCK = 256;

// Length code -> (base, extra bits)
inline constexpr int LEN_BASE[] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31, 35,43,51,59,
    67,83,99,115, 131,163,195,227, 258
};
inline constexpr int LEN_EXTRA[] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

// Distance code -> (base, extra bits)
inline constexpr int DIST_BASE[] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289, 16385,24577
};
inline constexpr int DIST_EXTRA[] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6, 7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

// Code length alphabet order
inline constexpr int CLEN_ORDER[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

inline int length_code(int len) noexcept {
    if (len < 3 || len > 258) return -1;
    if (len < 11) return len - 3 + 0;
    if (len < 19) return (len - 11) / 2 + 8;
    if (len < 35) return (len - 19) / 4 + 12;
    if (len < 67) return (len - 35) / 8 + 16;
    if (len < 131) return (len - 67) / 16 + 20;
    if (len < 258) return (len - 131) / 32 + 24;
    return 28; // len == 258
}

inline int distance_code(int dist) noexcept {
    if (dist < 1 || dist > 32768) return -1;
    if (dist < 5) return dist - 1;
    if (dist < 9) return (dist - 5) / 2 + 4;
    if (dist < 17) return (dist - 9) / 4 + 6;
    if (dist < 33) return (dist - 17) / 8 + 8;
    if (dist < 65) return (dist - 33) / 16 + 10;
    if (dist < 129) return (dist - 65) / 32 + 12;
    if (dist < 257) return (dist - 129) / 64 + 14;
    if (dist < 513) return (dist - 257) / 128 + 16;
    if (dist < 1025) return (dist - 513) / 256 + 18;
    if (dist < 2049) return (dist - 1025) / 512 + 20;
    if (dist < 4097) return (dist - 2049) / 1024 + 22;
    if (dist < 8193) return (dist - 4097) / 2048 + 24;
    if (dist < 16385) return (dist - 8193) / 4096 + 26;
    return (dist - 16385) / 8192 + 28;
}

inline int length_extra_bits(int code) noexcept { return LEN_EXTRA[code]; }
inline int length_base(int code) noexcept { return LEN_BASE[code]; }
inline int distance_extra_bits(int code) noexcept { return DIST_EXTRA[code]; }
inline int distance_base(int code) noexcept { return DIST_BASE[code]; }

} // namespace deflate
} // namespace fpng

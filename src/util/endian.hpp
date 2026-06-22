#pragma once

#include <bit>
#include <cstdint>
#include <concepts>
#include <algorithm>
#include <cstring>

namespace fpng {

inline constexpr std::endian native_order = std::endian::native;

inline constexpr bool is_big_endian() noexcept {
    return native_order == std::endian::big;
}

inline constexpr bool is_little_endian() noexcept {
    return native_order == std::endian::little;
}

inline uint16_t swap16(uint16_t v) noexcept {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

inline uint32_t swap32(uint32_t v) noexcept {
    return ((v >> 24) & 0xff) | ((v >> 8) & 0xff00) |
           ((v << 8) & 0xff0000) | ((v << 24) & 0xff000000u);
}

inline uint64_t swap64(uint64_t v) noexcept {
    return ((v >> 56) & 0xff) | ((v >> 40) & 0xff00) |
           ((v >> 24) & 0xff0000) | ((v >> 8) & 0xff000000) |
           ((v << 8) & 0xff00000000) | ((v << 24) & 0xff0000000000) |
           ((v << 40) & 0xff000000000000) | ((v << 56) & 0xff00000000000000ull);
}

inline uint16_t to_big16(uint16_t v) noexcept {
    if constexpr (native_order == std::endian::big) return v;
    return swap16(v);
}

inline uint32_t to_big32(uint32_t v) noexcept {
    if constexpr (native_order == std::endian::big) return v;
    return swap32(v);
}

inline uint16_t from_big16(uint16_t v) noexcept {
    return to_big16(v);
}

inline uint32_t from_big32(uint32_t v) noexcept {
    return to_big32(v);
}

template<typename T>
concept integral = std::integral<T> && !std::same_as<T, bool>;

inline void write_big16(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v & 0xff);
}

inline void write_big32(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    dst[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    dst[3] = static_cast<uint8_t>(v & 0xff);
}

inline uint16_t read_big16(const uint8_t* src) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8) | src[1]);
}

inline uint32_t read_big32(const uint8_t* src) noexcept {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

inline int32_t read_big32s(const uint8_t* src) noexcept {
    uint32_t u = read_big32(src);
    int32_t v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

} // namespace fpng

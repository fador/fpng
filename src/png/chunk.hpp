#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <optional>

namespace fpng {

using FourCC = std::array<char, 4>;

inline constexpr FourCC make_fourcc(char a, char b, char c, char d) noexcept {
    return {a, b, c, d};
}

namespace chunk_type {
    inline constexpr FourCC IHDR{'I','H','D','R'};
    inline constexpr FourCC PLTE{'P','L','T','E'};
    inline constexpr FourCC IDAT{'I','D','A','T'};
    inline constexpr FourCC IEND{'I','E','N','D'};
    inline constexpr FourCC tRNS{'t','R','N','S'};
    inline constexpr FourCC cHRM{'c','H','R','M'};
    inline constexpr FourCC gAMA{'g','A','M','A'};
    inline constexpr FourCC iCCP{'i','C','C','P'};
    inline constexpr FourCC sBIT{'s','B','I','T'};
    inline constexpr FourCC sRGB{'s','R','G','B'};
    inline constexpr FourCC cICP{'c','I','C','P'};
    inline constexpr FourCC mDCV{'m','D','C','V'};
    inline constexpr FourCC cLLI{'c','L','L','I'};
    inline constexpr FourCC tEXt{'t','E','X','t'};
    inline constexpr FourCC zTXt{'z','T','X','t'};
    inline constexpr FourCC iTXt{'i','T','X','t'};
    inline constexpr FourCC bKGD{'b','K','G','D'};
    inline constexpr FourCC hIST{'h','I','S','T'};
    inline constexpr FourCC pHYs{'p','H','Y','s'};
    inline constexpr FourCC sPLT{'s','P','L','T'};
    inline constexpr FourCC eXIf{'e','X','I','f'};
    inline constexpr FourCC tIME{'t','I','M','E'};
    inline constexpr FourCC acTL{'a','c','T','L'};
    inline constexpr FourCC fcTL{'f','c','T','L'};
    inline constexpr FourCC fdAT{'f','d','A','T'};
}

inline bool is_critical(const FourCC& type) noexcept {
    return (static_cast<uint8_t>(type[0]) & 0x20) == 0;
}

inline bool is_public(const FourCC& type) noexcept {
    return (static_cast<uint8_t>(type[1]) & 0x20) == 0;
}

inline bool is_safe_to_copy(const FourCC& type) noexcept {
    return (static_cast<uint8_t>(type[3]) & 0x20) == 0;
}

inline std::string fourcc_str(const FourCC& type) {
    return std::string(type.data(), 4);
}

inline bool operator==(const FourCC& a, const FourCC& b) noexcept {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

inline bool operator!=(const FourCC& a, const FourCC& b) noexcept {
    return !(a == b);
}

} // namespace fpng

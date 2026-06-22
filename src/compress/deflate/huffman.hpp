#pragma once

#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <array>

namespace fpng {

struct HuffmanCode {
    uint16_t code;
    uint8_t  bits;
};

// Builds optimal length-limited Huffman codes (max 15 bits per DEFLATE)
class HuffmanEncoder {
public:
    // Compute optimal code lengths using Package-Merge algorithm
    // frequencies: symbol frequencies (unused symbols = 0)
    // num_symbols: number of symbols
    // max_bits: maximum code length (15 for DEFLATE)
    // Returns array of code lengths indexed by symbol
    static std::vector<uint8_t> compute_lengths(
        const uint32_t* frequencies, size_t num_symbols, int max_bits = 15);

    // Convert code lengths to canonical Huffman codes
    static std::vector<HuffmanCode> lengths_to_codes(
        const uint8_t* lengths, size_t num_symbols);

    // Encode a Huffman tree for DEFLATE header (run-length encoded)
    // Returns the encoded tree + the number of code length codes
    static std::vector<uint8_t> encode_tree(
        const uint8_t* lengths, size_t num_symbols,
        std::array<uint8_t, deflate::MAX_CLEN_SYMS>& clen_lengths);

    // Count code length frequencies for the CLEN alphabet
    static void count_clen_freqs(
        const uint8_t* lengths, size_t num_symbols,
        uint32_t* clen_freqs);
};

// Reverse bits (for LSB-first DEFLATE output)
inline uint32_t reverse_bits_u32(uint32_t v, int num_bits) noexcept {
    uint32_t r = 0;
    for (int i = 0; i < num_bits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

} // namespace fpng

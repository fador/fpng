#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>

namespace fpng {

// Forward declarations for types used by deflater
enum class CompressionLevel {
    Store   = 0,
    Fast    = 1,
    Default = 6,
    Best    = 9,
    Ultra   = 12,
};

struct DeflateOptions {
    CompressionLevel level = CompressionLevel::Best;
    int iterations = 1;           // iterative LZ77 refinement passes
    bool optimal_parsing = true;   // use minimum-cost-path LZ77
    bool package_merge_huffman = true; // use Package-Merge for optimal Huffman
    bool block_splitting = true;  // optimal block boundary selection
};

std::vector<uint8_t> deflate_compress(std::span<const uint8_t> data,
                                       const DeflateOptions& opts = {});

// Zlib wrapper (RFC 1950)
std::vector<uint8_t> zlib_compress(std::span<const uint8_t> data,
                                    const DeflateOptions& opts = {});

} // namespace fpng

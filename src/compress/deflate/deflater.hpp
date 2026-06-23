#pragma once

#include "compress/deflate/constants.hpp"
#include "compress/deflate/lz77.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>

namespace fpng {

enum class CompressionLevel {
    Store   = 0,
    Fast    = 1,
    Default = 6,
    Best    = 9,
    Ultra   = 12,
};

struct DeflateOptions {
    CompressionLevel level = CompressionLevel::Best;
    int iterations = 1;
    bool optimal_parsing = false;
    bool adaptive_blocks = true;
    size_t max_block_size = 65536;
    int  chain_depth = 128;       // Hash chain walk limit (256-512 for ultra)
    bool verbose = false;
};

class Deflater {
public:
    Deflater() = default;

    // Compress data using DEFLATE (RFC 1951)
    std::vector<uint8_t> compress(std::span<const uint8_t> data,
                                   const DeflateOptions& opts = {});

    // Zlib wrapper (RFC 1950)
    std::vector<uint8_t> compress_zlib(std::span<const uint8_t> data,
                                        const DeflateOptions& opts = {});

private:
    struct BlockStats {
        uint32_t litlen_freq[deflate::MAX_LITLEN_SYMS] = {};
        uint32_t dist_freq[deflate::MAX_DIST_SYMS] = {};
        size_t num_tokens = 0;
        std::vector<LZ77Parser::Token> tokens;
    };

    void encode_stored_block(const uint8_t* data, size_t size,
                              bool is_last, std::vector<uint8_t>& out);

    void encode_dynamic_block(BlockStats& stats, bool is_last,
                               std::vector<uint8_t>& out);

    static uint32_t adler32(const uint8_t* data, size_t len);
};

// Convenience functions
std::vector<uint8_t> deflate_compress(std::span<const uint8_t> data,
                                       const DeflateOptions& opts = {});
std::vector<uint8_t> zlib_compress(std::span<const uint8_t> data,
                                    const DeflateOptions& opts = {});

} // namespace fpng

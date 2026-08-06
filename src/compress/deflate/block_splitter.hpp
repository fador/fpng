#pragma once

#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace fpng {

struct BlockSplit {
    size_t start_offset;
    size_t end_offset;
};

// Simple block splitter: splits at regular intervals or when
// data characteristics change
class BlockSplitter {
public:
    // Split data into blocks for optimal compression
    // Returns list of (start, end) offsets
    static std::vector<BlockSplit> split(
        const uint8_t* data, size_t size,
        size_t max_block_size = 8192);

    // Adaptive block splitting based on frequency changes (greedy, O(n))
    static std::vector<BlockSplit> split_greedy_adaptive(
        const uint8_t* data, size_t size,
        size_t min_block = 2048,
        size_t max_block = 8192,
        size_t max_blocks = 16);
};

} // namespace fpng

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
        size_t max_block_size = 65536);

    // Adaptive block splitting based on frequency changes
    static std::vector<BlockSplit> split_adaptive(
        const uint8_t* data, size_t size,
        size_t min_block = 1024,
        size_t max_block = 65536);
};

} // namespace fpng

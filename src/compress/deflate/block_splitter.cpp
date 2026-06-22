#include "compress/deflate/block_splitter.hpp"

#include <algorithm>
#include <cstring>

namespace fpng {

std::vector<BlockSplit> BlockSplitter::split(
    const uint8_t* data, size_t size, size_t max_block_size) {

    std::vector<BlockSplit> blocks;
    if (size == 0) return blocks;

    size_t pos = 0;
    while (pos < size) {
        size_t end = std::min(pos + max_block_size, size);
        blocks.push_back({pos, end});
        pos = end;
    }
    return blocks;
}

std::vector<BlockSplit> BlockSplitter::split_adaptive(
    const uint8_t* data, size_t size,
    size_t min_block, size_t max_block) {

    std::vector<BlockSplit> blocks;
    if (size <= min_block) {
        if (size > 0) blocks.push_back({0, size});
        return blocks;
    }

    // Simple adaptive split: track byte frequency histograms
    // and split when the distribution changes significantly
    size_t pos = 0;
    size_t block_start = 0;

    // Sliding window frequency counts
    uint32_t freq[256] = {};
    uint32_t recent_freq[256] = {};
    size_t recent_count = 0;

    constexpr size_t RECENT_WINDOW = 1024;

    while (pos < size) {
        uint8_t byte = data[pos];

        freq[byte]++;
        recent_freq[byte]++;
        recent_count++;

        // Check if we should split
        bool should_split = false;

        if (pos - block_start >= max_block) {
            should_split = true;
        } else if (pos - block_start >= min_block && recent_count >= RECENT_WINDOW) {
            // Compare recent distribution with overall
            size_t total = pos - block_start;
            if (total > 0) {
                double divergence = 0.0;
                for (int i = 0; i < 256; ++i) {
                    double p_overall = static_cast<double>(freq[i]) / total;
                    double p_recent = static_cast<double>(recent_freq[i]) / recent_count;
                    double diff = p_overall - p_recent;
                    divergence += diff * diff;
                }
                if (divergence > 0.01) {
                    should_split = true;
                }
            }
        }

        if (should_split && pos > block_start) {
            blocks.push_back({block_start, pos});
            block_start = pos;
            std::memset(freq, 0, sizeof(freq));
            std::memset(recent_freq, 0, sizeof(recent_freq));
            recent_count = 0;
        }

        // Rotate recent window
        if (recent_count >= RECENT_WINDOW * 2) {
            // Decay old entries
            for (int i = 0; i < 256; ++i)
                recent_freq[i] /= 2;
            recent_count /= 2;
        }

        ++pos;
    }

    if (pos > block_start) {
        blocks.push_back({block_start, pos});
    }

    return blocks;
}

} // namespace fpng

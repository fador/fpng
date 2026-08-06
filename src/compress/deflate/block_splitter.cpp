#include "compress/deflate/block_splitter.hpp"
#include "compress/deflate/constants.hpp"

#include <algorithm>
#include <vector>
#include <cmath>

namespace fpng {

std::vector<BlockSplit> BlockSplitter::split(
    const uint8_t* /*data*/, size_t size, size_t max_block_size) {

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

std::vector<BlockSplit> BlockSplitter::split_greedy_adaptive(
    const uint8_t* data, size_t size,
    size_t min_block, size_t max_block, size_t max_blocks) {

    if (size == 0) return {};
    if (size <= min_block * 2) return {{0, size}};

    constexpr size_t STRIDE = 1024;
    constexpr size_t WINDOW = 512;
    constexpr double THRESHOLD = 0.25;

    std::vector<size_t> splits;
    splits.push_back(0);

    double running_entropy = 0;
    size_t entropy_count = 0;

    for (size_t pos = min_block; pos + min_block < size; pos += STRIDE) {
        size_t win_start = (pos >= WINDOW) ? pos - WINDOW : 0;
        size_t win_end = std::min(pos + WINDOW, size);
        size_t win_size = win_end - win_start;
        if (win_size < 64) continue;

        uint32_t freq[256] = {};
        for (size_t i = win_start; i < win_end; ++i)
            freq[data[i]]++;

        double entropy = 0;
        double inv = 1.0 / win_size;
        for (int i = 0; i < 256; ++i) {
            if (freq[i] > 0) {
                double p = freq[i] * inv;
                entropy -= p * std::log2(p);
            }
        }

        if (entropy_count > 0 && entropy > 0) {
            double ratio = entropy / running_entropy;
            if (ratio > 1.0 + THRESHOLD || ratio < 1.0 - THRESHOLD) {
                splits.push_back(pos);
                if (splits.size() >= max_blocks) break;
            }
        }

        // Update running average entropy
        double alpha = 1.0 / (entropy_count + 1);
        running_entropy = running_entropy * (1.0 - alpha) + entropy * alpha;
        ++entropy_count;
    }

    splits.push_back(size);

    // Ensure minimum block interval
    std::vector<size_t> filtered;
    filtered.push_back(0);
    for (size_t i = 1; i + 1 < splits.size(); ++i) {
        if (splits[i] - filtered.back() >= min_block)
            filtered.push_back(splits[i]);
    }
    filtered.push_back(size);

    // Convert to blocks, ensuring max_block
    std::vector<BlockSplit> blocks;
    for (size_t i = 0; i + 1 < filtered.size(); ++i) {
        size_t start = filtered[i];
        size_t end = filtered[i + 1];
        while (start < end) {
            size_t chunk = std::min(max_block, end - start);
            blocks.push_back({start, start + chunk});
            start += chunk;
        }
    }

    if (blocks.empty())
        blocks.push_back({0, size});

    return blocks;
}

} // namespace fpng

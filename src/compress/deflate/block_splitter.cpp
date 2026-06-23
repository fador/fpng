#include "compress/deflate/block_splitter.hpp"
#include "compress/deflate/huffman.hpp"
#include "compress/deflate/constants.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include <limits>
#include <cmath>

namespace fpng {

namespace {

// Estimate the compressed bit cost of a data block using fixed Huffman.
// This is fast (no tree building) and serves as a proxy for actual cost.
size_t estimate_block_cost(const uint8_t* data, size_t size) {
    if (size < 3) return size * 8 + 7; // tiny: stored block cost

    // Count literal frequencies
    uint32_t lit_freq[256] = {};
    for (size_t i = 0; i < size; ++i)
        lit_freq[data[i]]++;

    // Fixed Huffman: literals 0-143 = 8 bits, 144-255 = 9 bits
    size_t total_bits = 3; // block header
    for (int i = 0; i < 256; ++i) {
        if (lit_freq[i] > 0) {
            int bits = (i <= 143) ? 8 : 9;
            total_bits += lit_freq[i] * bits;
        }
    }

    // Estimate LZ77 match savings: scan for runs of repeated bytes
    size_t saved_bits = 0;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == data[i+1] && data[i+1] == data[i+2]) {
            size_t run = 3;
            while (i + run < size && data[i + run] == data[i]) ++run;
            if (run >= 4) {
                // Match of length `run` saves ~(run - 3) * 8 - 15 bits
                saved_bits += (run - 3) * 8;
                i += run - 1;
            }
        }
    }

    return (total_bits > saved_bits) ? (total_bits - saved_bits) : total_bits;
}

} // anonymous namespace

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

std::vector<BlockSplit> BlockSplitter::split_adaptive(
    const uint8_t* data, size_t size,
    size_t min_block, size_t max_block) {

    // For very small data, use one block
    if (size <= min_block * 2) {
        if (size > 0) return {{0, size}};
        return {};
    }

    // Generate candidate split points: every N bytes + at frequency shifts
    std::vector<size_t> candidates;
    candidates.push_back(0);

    constexpr size_t STRIDE = 512;
    for (size_t i = STRIDE; i < size; i += STRIDE) {
        if (i >= min_block && (size - i) >= min_block)
            candidates.push_back(i);
    }
    candidates.push_back(size);

    // Remove candidates that are too close
    std::vector<size_t> filtered;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i == 0 || i == candidates.size() - 1 ||
            candidates[i] - filtered.back() >= min_block) {
            filtered.push_back(candidates[i]);
        }
    }
    candidates = std::move(filtered);
    if (candidates.size() <= 2) return {{0, size}};

    // DP: find optimal splits
    // cost[i] = best cost for data[0..candidates[i])
    constexpr size_t INF = std::numeric_limits<size_t>::max();
    std::vector<size_t> dp(candidates.size(), INF);
    std::vector<int> prev(candidates.size(), -1);
    dp[0] = 0;

    for (size_t j = 1; j < candidates.size(); ++j) {
        for (size_t i = 0; i < j; ++i) {
            size_t block_size = candidates[j] - candidates[i];
            if (block_size > max_block) continue;
            if (block_size < min_block && i > 0) continue;

            size_t cost = estimate_block_cost(data + candidates[i], block_size);
            size_t total = dp[i] + cost;
            if (total < dp[j]) {
                dp[j] = total;
                prev[j] = static_cast<int>(i);
            }
        }
    }

    // Backtrack
    std::vector<BlockSplit> blocks;
    int j = static_cast<int>(candidates.size()) - 1;
    while (j > 0) {
        int i = prev[j];
        if (i < 0) break;
        blocks.push_back({candidates[static_cast<size_t>(i)],
                          candidates[static_cast<size_t>(j)]});
        j = i;
    }
    std::reverse(blocks.begin(), blocks.end());

    if (blocks.empty())
        blocks.push_back({0, size});

    return blocks;
}

} // namespace fpng

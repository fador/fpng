#include "compress/filter_optimizer.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <map>
#include <array>
#include <random>

namespace fpng {

namespace {

uint64_t sum_abs(const uint8_t* data, size_t size) {
    uint64_t sum = 0;
    for (size_t i = 0; i < size; ++i) {
        int8_t v = static_cast<int8_t>(data[i]);
        sum += static_cast<uint64_t>(std::abs(static_cast<int>(v)));
    }
    return sum;
}

double byte_entropy(const uint8_t* data, size_t size) {
    uint32_t counts[256] = {};
    for (size_t i = 0; i < size; ++i) counts[data[i]]++;
    double entropy = 0;
    double inv = 1.0 / size;
    for (int i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            double p = counts[i] * inv;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

double filter_cost(FilterType ft, const uint8_t* src, size_t byte_width,
                   size_t pixel_stride, const uint8_t* prev) {
    std::vector<uint8_t> filtered(byte_width + 1);
    filter_scanline(ft, src, filtered.data(), pixel_stride, byte_width, prev);
    return byte_entropy(filtered.data() + 1, byte_width);
}

// Compress trial data with a fast (greedy + fixed Huffman) encoder and return
// size. Previously this used CompressionLevel::Store, which returns the same
// byte count for every candidate filter — making the brute-force filter search
// a no-op that always selected None.
size_t trial_compress_size(const std::vector<uint8_t>& data) {
    DeflateOptions dopts;
    dopts.level = CompressionLevel::Fast;
    dopts.iterations = 1;
    dopts.adaptive_blocks = false;
    dopts.chain_depth = 32;
    dopts.max_block_size = 65535;
    return deflate_compress(data, dopts).size();
}

} // anonymous namespace

std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts) {
    size_t raw_ss = img.raw_scanline_size();
    size_t bpp = img.bytes_per_pixel();
    size_t height = img.height;

    if (height == 0) return {};

    std::vector<FilterType> result(height, FilterType::None);

    if (opts.level == 0) {
        // MinSum heuristic
        std::vector<uint8_t> prev(raw_ss, 0);
        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            std::vector<uint8_t> filtered(raw_ss + 1);

            FilterType best = FilterType::None;
            uint64_t best_cost = std::numeric_limits<uint64_t>::max();

            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);
                filter_scanline(type, src, filtered.data(), bpp, raw_ss,
                                 y > 0 ? prev.data() : nullptr);
                uint64_t cost = sum_abs(filtered.data() + 1, raw_ss);
                if (cost < best_cost) { best_cost = cost; best = type; }
            }
            result[y] = best;
            std::memcpy(prev.data(), src, raw_ss);
        }
    } else if (opts.level <= 2) {
        // Entropy-based heuristic
        std::vector<uint8_t> prev(raw_ss, 0);
        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            FilterType best = FilterType::None;
            double best_cost = std::numeric_limits<double>::max();
            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);
                double cost = filter_cost(type, src, raw_ss, bpp,
                                           y > 0 ? prev.data() : nullptr);
                if (cost < best_cost) { best_cost = cost; best = type; }
            }
            result[y] = best;
            std::memcpy(prev.data(), src, raw_ss);
        }
    } else if (opts.level <= 4) {
        // Windowed brute-force with actual compression
        int window = std::max(1, opts.window_size);
        std::vector<uint8_t> prev(raw_ss, 0);

        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            FilterType best = FilterType::None;
            size_t best_size = std::numeric_limits<size_t>::max();

            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);
                std::vector<uint8_t> trial;
                std::vector<uint8_t> trial_prev = prev;
                size_t end = std::min(y + static_cast<size_t>(window), height);

                for (size_t wy = y; wy < end; ++wy) {
                    const uint8_t* row = img.pixels.data() + wy * raw_ss;
                    std::vector<uint8_t> filtered(raw_ss + 1);
                    FilterType row_ft = (wy == y) ? type : FilterType::None;
                    filter_scanline(row_ft, row, filtered.data(), bpp, raw_ss,
                                     wy > 0 ? trial_prev.data() : nullptr);
                    trial.insert(trial.end(), filtered.data(), filtered.data() + raw_ss + 1);
                    std::memcpy(trial_prev.data(), row, raw_ss);
                }
                size_t sz = trial_compress_size(trial);
                if (sz < best_size) { best_size = sz; best = type; }
            }
            result[y] = best;
            std::memcpy(prev.data(), src, raw_ss);
        }
    } else {
        // Levels 5+: Exact Viterbi Dynamic Programming
        // Minimizes: sum(row_min_sum[y][filter]) + switches * penalty
        // Solved to guaranteed global mathematical optimality in O(25 * H) time.
        std::vector<std::array<uint64_t, 5>> row_min_sum(height);
        {
            std::vector<uint8_t> prev_src(raw_ss, 0);
            std::vector<uint8_t> row(raw_ss + 1);
            for (size_t y = 0; y < height; ++y) {
                const uint8_t* src = img.pixels.data() + y * raw_ss;
                for (int ft = 0; ft < 5; ++ft) {
                    filter_scanline(static_cast<FilterType>(ft), src, row.data(),
                                    bpp, raw_ss, y > 0 ? prev_src.data() : nullptr);
                    row_min_sum[y][ft] = sum_abs(row.data() + 1, raw_ss);
                }
                std::memcpy(prev_src.data(), src, raw_ss);
            }
        }

        uint64_t penalty = (opts.level >= 7 || opts.use_genetic) ? 1000 : 500;

        uint64_t dp[5];
        for (int ft = 0; ft < 5; ++ft) dp[ft] = row_min_sum[0][ft];

        std::vector<std::array<uint8_t, 5>> parent(height);

        for (size_t y = 1; y < height; ++y) {
            uint64_t next_dp[5];
            for (int cur = 0; cur < 5; ++cur) {
                uint64_t best_c = std::numeric_limits<uint64_t>::max();
                uint8_t best_p = 0;
                for (int prev = 0; prev < 5; ++prev) {
                    uint64_t c = dp[prev] + (prev != cur ? penalty : 0);
                    if (c < best_c) {
                        best_c = c;
                        best_p = static_cast<uint8_t>(prev);
                    }
                }
                next_dp[cur] = best_c + row_min_sum[y][cur];
                parent[y][cur] = best_p;
            }
            for (int ft = 0; ft < 5; ++ft) dp[ft] = next_dp[ft];
        }

        int best_last = 0;
        uint64_t min_total = dp[0];
        for (int ft = 1; ft < 5; ++ft) {
            if (dp[ft] < min_total) {
                min_total = dp[ft];
                best_last = ft;
            }
        }

        int cur = best_last;
        for (size_t y = height; y-- > 0;) {
            result[y] = static_cast<FilterType>(cur);
            if (y > 0) cur = parent[y][cur];
        }
    }


    return result;
}

} // namespace fpng

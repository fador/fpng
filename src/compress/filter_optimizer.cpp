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

// Compress trial data with a fast encoder and return bit-accurate size.
size_t trial_compress_size(std::span<const uint8_t> data, int row_stride) {
    DeflateOptions dopts;
    dopts.level = CompressionLevel::Fast;
    dopts.iterations = 1;
    dopts.adaptive_blocks = false;
    dopts.chain_depth = 64;
    dopts.max_block_size = 65535;
    dopts.row_stride = row_stride;
    return deflate_compress(data, dopts).size();
}

} // anonymous namespace

std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts) {
    size_t raw_ss = img.raw_scanline_size();
    size_t bpp = img.bytes_per_pixel();
    size_t height = img.height;

    if (height == 0 || raw_ss == 0) return {};

    std::vector<FilterType> result(height, FilterType::None);

    if (opts.level == 0) {
        // MinSum heuristic
        std::vector<uint8_t> prev(raw_ss, 0);
        std::vector<uint8_t> filtered(raw_ss + 1);
        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;

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
    } else if (opts.level == 1) {
        // Cumulative / Rolling Cross-Entropy + Local LZ77 repeat bonus:
        // Chooses each scanline's filter to minimize the marginal Shannon bit cost
        // against the running byte distribution of preceding scanlines, while
        // rewarding 3-byte and 4-byte runs that LZ77 can match.
        uint32_t cum_freq[256];
        std::fill(std::begin(cum_freq), std::end(cum_freq), 1u); // Laplace prior
        uint64_t cum_total = 256;

        std::vector<uint8_t> prev(raw_ss, 0);
        std::vector<uint8_t> prev_filtered(raw_ss + 1, 0);
        std::vector<std::array<uint8_t, 5>> cand_rows;
        std::array<std::vector<uint8_t>, 5> filtered_cands;
        for (int ft = 0; ft < 5; ++ft) filtered_cands[ft].resize(raw_ss + 1);

        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            FilterType best = FilterType::None;
            double best_bits = std::numeric_limits<double>::max();

            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);
                auto& frow = filtered_cands[ft];
                filter_scanline(type, src, frow.data(), bpp, raw_ss,
                                 y > 0 ? prev.data() : nullptr);

                // Compute marginal bit cost under cumulative distribution + current row
                uint32_t row_counts[256] = {};
                size_t lz_bonus_bytes = 0;
                const uint8_t* d = frow.data() + 1;
                const uint8_t* pd = prev_filtered.data() + 1;

                for (size_t i = 0; i < raw_ss; ++i) {
                    row_counts[d[i]]++;
                    // Check distance-1, distance-bpp, and vertical (prev_filtered) 3-byte repeats
                    if (i + 2 < raw_ss) {
                        if ((i >= 1 && d[i] == d[i - 1] && d[i + 1] == d[i] && d[i + 2] == d[i]) ||
                            (i >= bpp && d[i] == d[i - bpp] && d[i + 1] == d[i - bpp + 1] && d[i + 2] == d[i - bpp + 2]) ||
                            (y > 0 && d[i] == pd[i] && d[i + 1] == pd[i + 1] && d[i + 2] == pd[i + 2])) {
                            ++lz_bonus_bytes;
                        }
                    }
                }

                double bits = 0.0;
                double denom = static_cast<double>(cum_total + raw_ss);
                for (int b = 0; b < 256; ++b) {
                    if (row_counts[b] > 0) {
                        double p = static_cast<double>(cum_freq[b] + row_counts[b]) / denom;
                        bits -= static_cast<double>(row_counts[b]) * std::log2(p);
                    }
                }
                // Reward LZ77-matchable bytes
                bits -= static_cast<double>(lz_bonus_bytes) * 1.5;

                if (bits < best_bits) {
                    best_bits = bits;
                    best = type;
                }
            }

            result[y] = best;
            const auto& win_row = filtered_cands[static_cast<int>(best)];
            for (size_t i = 1; i <= raw_ss; ++i) {
                cum_freq[win_row[i]]++;
            }
            cum_total += raw_ss;

            // Periodically decay histogram to stay local to the 32KB-64KB DEFLATE window
            if ((y & 31) == 31) {
                cum_total = 0;
                for (int b = 0; b < 256; ++b) {
                    cum_freq[b] = (cum_freq[b] >> 1) + 1;
                    cum_total += cum_freq[b];
                }
            }

            prev_filtered = win_row;
            std::memcpy(prev.data(), src, raw_ss);
        }
    } else if (opts.level == 2) {
        // Per-scanline Shannon entropy heuristic
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
        // History-aware windowed trial compression:
        // Keep a sliding buffer of recently filtered scanlines so that trial
        // DEFLATE compression sees cross-row LZ77 matches and realistic symbol
        // frequencies, and use the candidate filter (instead of hardcoded None)
        // across the lookahead window.
        int window = std::max(1, opts.window_size);
        size_t stride = raw_ss + 1;
        size_t max_hist_rows = std::clamp<size_t>(4096 / std::max<size_t>(1, stride), 4, 32);

        std::vector<uint8_t> history;
        history.reserve((max_hist_rows + window) * stride);
        std::vector<uint8_t> prev(raw_ss, 0);
        std::array<std::vector<uint8_t>, 5> row_cands;
        for (int ft = 0; ft < 5; ++ft) row_cands[ft].resize(stride);

        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            FilterType best = FilterType::None;
            size_t best_size = std::numeric_limits<size_t>::max();
            uint64_t best_tie_sum = std::numeric_limits<uint64_t>::max();

            size_t hist_base = history.size();
            size_t end = std::min(y + static_cast<size_t>(window), height);

            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);
                filter_scanline(type, src, row_cands[ft].data(), bpp, raw_ss,
                                 y > 0 ? prev.data() : nullptr);
                uint64_t tie_sum = sum_abs(row_cands[ft].data() + 1, raw_ss);

                history.resize(hist_base);
                history.insert(history.end(), row_cands[ft].begin(), row_cands[ft].end());

                // Lookahead rows use the same filter `type` to test pattern continuity
                const uint8_t* trial_prev = src;
                std::vector<uint8_t> look_buf(stride);
                for (size_t wy = y + 1; wy < end; ++wy) {
                    const uint8_t* wrow = img.pixels.data() + wy * raw_ss;
                    filter_scanline(type, wrow, look_buf.data(), bpp, raw_ss, trial_prev);
                    history.insert(history.end(), look_buf.begin(), look_buf.end());
                    trial_prev = wrow;
                }

                size_t sz = trial_compress_size(history, static_cast<int>(stride));
                if (sz < best_size || (sz == best_size && tie_sum < best_tie_sum)) {
                    best_size = sz;
                    best_tie_sum = tie_sum;
                    best = type;
                }
            }

            result[y] = best;
            history.resize(hist_base);
            const auto& win_row = row_cands[static_cast<int>(best)];
            history.insert(history.end(), win_row.begin(), win_row.end());
            if (history.size() > max_hist_rows * stride) {
                size_t drop = history.size() - max_hist_rows * stride;
                history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(drop));
            }
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

        // Scale switch penalty proportionally to scanline width so wide and narrow
        // images get consistent regularization.
        uint64_t base_pen = std::max<uint64_t>(16, raw_ss / 2);
        uint64_t penalty = (opts.level >= 7 || opts.use_genetic) ? (base_pen * 2) : base_pen;

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

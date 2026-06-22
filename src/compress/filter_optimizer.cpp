#include "compress/filter_optimizer.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace fpng {

namespace {

// Compute sum of absolute byte values (proxy for compressibility)
uint64_t sum_abs(const uint8_t* data, size_t size) {
    uint64_t sum = 0;
    for (size_t i = 0; i < size; ++i) sum += data[i];
    return sum;
}

// Compute Shannon entropy of byte values (better proxy for compressibility)
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

// Apply a single filter to a row and measure the cost
double filter_cost(FilterType ft, const uint8_t* src, size_t byte_width,
                   size_t pixel_stride, const uint8_t* prev) {
    std::vector<uint8_t> filtered(byte_width + 1);
    filter_scanline(ft, src, filtered.data(), pixel_stride, byte_width, prev);
    return byte_entropy(filtered.data() + 1, byte_width);
}

} // anonymous namespace

std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts) {
    size_t raw_ss = img.raw_scanline_size();
    size_t bpp = img.bytes_per_pixel();
    size_t height = img.height;

    if (height == 0) return {};

    std::vector<FilterType> result(height, FilterType::None);

    if (opts.level == 0) {
        // MinSum heuristic: pick filter with smallest sum of absolute values
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
                if (cost < best_cost) {
                    best_cost = cost;
                    best = type;
                }
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
                if (cost < best_cost) {
                    best_cost = cost;
                    best = type;
                }
            }

            result[y] = best;
            std::memcpy(prev.data(), src, raw_ss);
        }
    } else {
        // Brute-force with windowed lookahead
        int window = std::max(1, opts.window_size);
        std::vector<uint8_t> prev(raw_ss, 0);

        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;

            FilterType best = FilterType::None;
            size_t best_size = std::numeric_limits<size_t>::max();

            // Try each filter and actually compress a window to measure size
            for (int ft = 0; ft <= 4; ++ft) {
                FilterType type = static_cast<FilterType>(ft);

                // Simulate filtering a window of rows and measure compressed size
                std::vector<uint8_t> trial_data;
                std::vector<uint8_t> trial_prev = prev;

                size_t end_y = std::min(y + static_cast<size_t>(window), height);
                for (size_t wy = y; wy < end_y; ++wy) {
                    const uint8_t* row = img.pixels.data() + wy * raw_ss;
                    std::vector<uint8_t> filtered(raw_ss + 1);

                    FilterType row_ft = (wy == y) ? type : FilterType::None;
                    filter_scanline(row_ft, row, filtered.data(), bpp, raw_ss,
                                     wy > 0 ? trial_prev.data() : nullptr);
                    trial_data.insert(trial_data.end(),
                                      filtered.data(), filtered.data() + raw_ss + 1);
                    std::memcpy(trial_prev.data(), row, raw_ss);
                }

                // Compress trial data using stored blocks (fast)
                DeflateOptions def_opts;
                def_opts.level = CompressionLevel::Store;
                auto compressed = deflate_compress(trial_data, def_opts);

                if (compressed.size() < best_size) {
                    best_size = compressed.size();
                    best = type;
                }
            }

            result[y] = best;
            std::memcpy(prev.data(), src, raw_ss);
        }
    }

    return result;
}

} // namespace fpng

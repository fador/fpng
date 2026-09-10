#pragma once

#include "image/image.hpp"
#include "compress/deflate/deflater.hpp"

#include <vector>
#include <cstdint>
#include <string>

namespace fpng {

struct CompressOptions {
    int level = 9;              // 0=fast, 5=balanced, 9=max
    bool strip_ancillary = false;
    bool keep_color_info = true;
    bool multi_strategy = true;  // try multiple strategies, pick best
    int num_threads = 0;        // 0 = auto (hardware concurrency)
    bool verbose = false;
};

struct CompressResult {
    std::vector<uint8_t> data;
    double time_seconds = 0;
    size_t original_size = 0;
    std::string strategy_name;
};

// Single-strategy compression (used internally and as fallback)
CompressResult compress_single(const Image& img, const CompressOptions& opts);

// Multi-strategy compression: tries multiple filter+deflate combos
CompressResult compress(const Image& img, const CompressOptions& opts = {});

// Strategy descriptor for one compression attempt
struct Strategy {
    int filter_level;
    CompressionLevel deflate_level;
    int deflate_iterations;
    bool alpha_zero;
    bool palette_sort;
    std::string name;
    bool color_reduce = false; // try RGB->gray, truecolor->indexed, 16->8-bit
};

// Get the list of strategies to try for a given option level
std::vector<Strategy> get_strategies(int level);

// Run one strategy on an image (must be pre-processed as needed)
std::vector<uint8_t> run_strategy(const Image& img, const Strategy& s);

} // namespace fpng

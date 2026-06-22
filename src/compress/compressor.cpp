#include "compress/compressor.hpp"
#include "compress/deflate/deflater.hpp"
#include "compress/filter_optimizer.hpp"
#include "png/writer.hpp"
#include "png/reader.hpp"
#include "preprocess/preprocessor.hpp"
#include "preprocess/alpha_optimizer.hpp"
#include "preprocess/palette_sorter.hpp"
#include "util/timer.hpp"

#include <future>
#include <algorithm>
#include <thread>
#include <iostream>
#include <cstring>

namespace fpng {

std::vector<Strategy> get_strategies(int level) {
    std::vector<Strategy> strategies;

    if (level <= 0) {
        // Fast: just one strategy
        strategies.push_back({0, CompressionLevel::Fast, 1, true, false, "fast"});
        return strategies;
    }

    if (level <= 3) {
        // Balanced: a few strategies
        strategies.push_back({2, CompressionLevel::Default, 1, true, false, "balanced-a"});
        strategies.push_back({2, CompressionLevel::Best, 1, true, false, "balanced-b"});
        return strategies;
    }

    if (level <= 6) {
        // Good: more strategies
        strategies.push_back({2, CompressionLevel::Default, 1, true, false, "good-a"});
        strategies.push_back({2, CompressionLevel::Best, 1, true, false, "good-b"});
        strategies.push_back({2, CompressionLevel::Best, 2, true, false, "good-c"});
        strategies.push_back({3, CompressionLevel::Best, 2, true, false, "good-d"});
        return strategies;
    }

    // Maximum: try many strategies
    strategies.push_back({2, CompressionLevel::Default, 1, true, false,  "max-01"});
    strategies.push_back({2, CompressionLevel::Best, 1, true, false,     "max-02"});
    strategies.push_back({2, CompressionLevel::Best, 2, true, false,     "max-03"});
    strategies.push_back({2, CompressionLevel::Ultra, 2, true, false,    "max-04"});
    strategies.push_back({3, CompressionLevel::Best, 1, true, false,     "max-05"});
    strategies.push_back({3, CompressionLevel::Best, 2, true, false,     "max-06"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false,    "max-07"});

    // Alpha-zero off variants
    strategies.push_back({2, CompressionLevel::Best, 2, false, false,    "max-08"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, false, false,   "max-09"});

    // Palette-sort variants
    strategies.push_back({2, CompressionLevel::Best, 2, true, true,      "max-10"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, true,     "max-11"});

    return strategies;
}

std::vector<uint8_t> run_strategy(const Image& img, const Strategy& s) {
    Image work = img;

    // Pre-processing
    if (s.alpha_zero) {
        alpha_optimize(work);
    }
    if (s.palette_sort) {
        sort_palette(work);
    }

    // Filter optimization
    FilterOptions fopts;
    fopts.level = s.filter_level;
    fopts.window_size = std::min(3, s.filter_level);
    auto filters = optimize_filters(work, fopts);

    // Build filtered data
    size_t raw_ss = work.raw_scanline_size();
    size_t bpp = work.bytes_per_pixel();

    std::vector<uint8_t> filtered;
    filtered.reserve((raw_ss + 1) * work.height);

    std::vector<uint8_t> prev(raw_ss, 0);

    for (size_t y = 0; y < work.height; ++y) {
        const uint8_t* src = work.pixels.data() + y * raw_ss;
        std::vector<uint8_t> row(raw_ss + 1);
        FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
        filter_scanline(ft, src, row.data(), bpp, raw_ss,
                         y > 0 ? prev.data() : nullptr);
        filtered.insert(filtered.end(), row.begin(), row.end());
        std::memcpy(prev.data(), src, raw_ss);
    }

    // Compress
    DeflateOptions dopts;
    dopts.level = s.deflate_level;
    dopts.iterations = s.deflate_iterations;
    dopts.optimal_parsing = (s.deflate_iterations > 1);

    return zlib_compress(filtered, dopts);
}

CompressResult compress_single(const Image& img, const CompressOptions& opts) {
    Timer t;

    Image work = img;
    // Don't pre-process - preserve constant channels for filter efficiency

    auto strats = get_strategies(std::min(opts.level, 6));
    Strategy s = strats[0];
    if (opts.level >= 9 && strats.size() > 3) s = strats[3];

    // Re-apply strategy-specific pre-processing
    if (s.alpha_zero) alpha_optimize(work);
    if (s.palette_sort) sort_palette(work);

    FilterOptions fopts;
    fopts.level = s.filter_level;
    fopts.window_size = std::min(3, s.filter_level);
    auto filters = optimize_filters(work, fopts);

    DeflateOptions dopts;
    dopts.level = s.deflate_level;
    dopts.iterations = s.deflate_iterations;
    dopts.optimal_parsing = (s.deflate_iterations > 1);

    WriteOptions wopts;
    wopts.filters = filters;
    wopts.deflate = dopts;

    PNGWriter writer;
    auto png_out = writer.write(work, wopts);

    CompressResult result;
    result.data = std::move(png_out);
    result.time_seconds = t.elapsed_seconds();
    result.original_size = img.pixels.size();
    result.strategy_name = s.name;
    return result;
}

CompressResult compress(const Image& img, const CompressOptions& opts) {
    if (!opts.multi_strategy) {
        return compress_single(img, opts);
    }

    Timer total;

    // Use the original image - don't pre-process
    // (alpha stripping can hurt compression since constant channels
    //  compress extremely well with Up/Paeth filters)
    Image base = img;

    auto strategies = get_strategies(opts.level);

    if (opts.verbose) {
        std::cout << "Trying " << strategies.size() << " strategies...\n";
    }

    // Run strategies (parallel or sequential)
    int threads = opts.num_threads;
    if (threads <= 0) threads = std::max(1u, std::thread::hardware_concurrency());

    struct TrialResult {
        std::vector<uint8_t> data;
        std::string name;
        size_t size;
    };

    std::vector<std::future<TrialResult>> futures;
    futures.reserve(strategies.size());

    size_t running = 0;
    size_t next_idx = 0;

    auto run_trial = [&base](Strategy s) -> TrialResult {
        Timer t;
        auto deflated = run_strategy(base, s);
        // Wrap in minimal PNG wrapper
        // Just store the deflated data size for comparison
        TrialResult tr;
        tr.data = std::move(deflated);
        tr.name = s.name;
        tr.size = tr.data.size();
        return tr;
    };

    // Launch initial batch
    while (running < static_cast<size_t>(threads) && next_idx < strategies.size()) {
        futures.push_back(std::async(std::launch::async,
            run_trial, strategies[next_idx]));
        ++running;
        ++next_idx;
    }

    // Collect results and launch more
    std::vector<TrialResult> results;
    results.reserve(strategies.size());

    for (size_t i = 0; i < futures.size(); ++i) {
        auto tr = futures[i].get();
        if (opts.verbose) {
            std::cout << "  " << tr.name << ": " << tr.size << " bytes\n";
        }
        results.push_back(std::move(tr));

        // Launch next if available
        if (next_idx < strategies.size()) {
            futures.push_back(std::async(std::launch::async,
                run_trial, strategies[next_idx]));
            ++next_idx;
        }
    }

    // Find best
    auto best = std::min_element(results.begin(), results.end(),
        [](const TrialResult& a, const TrialResult& b) { return a.size < b.size; });

    // Build the PNG using the best strategy
    Strategy best_strat = strategies[best - results.begin()];

    if (opts.verbose) {
        std::cout << "Best: " << best_strat.name << " (" << best->size << " bytes)\n";
    }

    // Re-create image
    Image work2 = img;
    // Don't alpha-optimize (strip hurts compression of constant channels)
    if (best_strat.palette_sort) sort_palette(work2);

    // Compute filters with the winning strategy
    FilterOptions fopts;
    fopts.level = best_strat.filter_level;
    fopts.window_size = std::min(3, best_strat.filter_level);
    auto filters = optimize_filters(work2, fopts);

    DeflateOptions dopts;
    dopts.level = best_strat.deflate_level;
    dopts.iterations = best_strat.deflate_iterations;
    dopts.optimal_parsing = (best_strat.deflate_iterations > 1);

    WriteOptions wopts;
    wopts.filters = filters;
    wopts.deflate = dopts;

    PNGWriter writer;
    auto png_out = writer.write(work2, wopts);

    CompressResult result;
    result.data = std::move(png_out);
    result.time_seconds = total.elapsed_seconds();
    result.original_size = img.pixels.size();
    result.strategy_name = best_strat.name;
    return result;
}

} // namespace fpng

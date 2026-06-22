#include "compress/compressor.hpp"
#include "compress/deflate/deflater.hpp"
#include "compress/filter_optimizer.hpp"
#include "png/writer.hpp"
#include "png/reader.hpp"
#include "preprocess/preprocessor.hpp"
#include "preprocess/alpha_optimizer.hpp"
#include "preprocess/palette_sorter.hpp"
#include "preprocess/content_analyzer.hpp"
#include "util/timer.hpp"

#include <algorithm>
#include <thread>
#include <iostream>
#include <cstring>

namespace fpng {

std::vector<Strategy> get_strategies(int level) {
    std::vector<Strategy> strategies;

    if (level <= 0) {
        // Fast: just one strategy
        strategies.push_back({0, CompressionLevel::Fast, 1, true, false, false, "fast"});
        return strategies;
    }

    if (level <= 3) {
        // Balanced: a few strategies
        strategies.push_back({2, CompressionLevel::Default, 1, true, false, false, "balanced-a"});
        strategies.push_back({2, CompressionLevel::Best, 1, true, false, false, "balanced-b"});
        return strategies;
    }

    if (level <= 6) {
        // Good: more strategies
        strategies.push_back({2, CompressionLevel::Default, 1, true, false, false, "good-a"});
        strategies.push_back({2, CompressionLevel::Best, 1, true, false, false, "good-b"});
        strategies.push_back({2, CompressionLevel::Best, 2, true, false, false, "good-c"});
        strategies.push_back({3, CompressionLevel::Best, 2, true, false, false, "good-d"});
        return strategies;
    }

    // Maximum: try many strategies including GA filter optimization
    strategies.push_back({2, CompressionLevel::Default, 1, true, false, false, "max-01"});
    strategies.push_back({2, CompressionLevel::Best, 1, true, false, false, "max-02"});
    strategies.push_back({2, CompressionLevel::Best, 2, true, false, false, "max-03"});
    strategies.push_back({2, CompressionLevel::Ultra, 2, true, false, false, "max-04"});
    strategies.push_back({3, CompressionLevel::Best, 1, true, false, false, "max-05"});
    strategies.push_back({3, CompressionLevel::Best, 2, true, false, false, "max-06"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false, false, "max-07"});

    // Alpha-zero off variants
    strategies.push_back({2, CompressionLevel::Best, 2, false, false, false, "max-08"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, false, false, false, "max-09"});

    // Palette-sort variants
    strategies.push_back({2, CompressionLevel::Best, 2, true, true, false, "max-10"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, true, false, "max-11"});

    // GA filter optimization (high-effort)
    strategies.push_back({5, CompressionLevel::Best, 2, true, false, false, "max-12"});
    strategies.push_back({7, CompressionLevel::Best, 2, true, false, false, "max-13"});
    strategies.push_back({7, CompressionLevel::Ultra, 3, true, false, false, "max-14"});

    // BT match finder variants (exhaustive matching)
    strategies.push_back({2, CompressionLevel::Best, 2, true, false, true,  "max-15"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false, true, "max-16"});

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

    // Deflate
    DeflateOptions dopts;
    dopts.level = s.deflate_level;
    dopts.iterations = s.deflate_iterations;
    dopts.optimal_parsing = (s.deflate_iterations > 1);
    dopts.bt_match_finder = s.bt_match;

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

    // Analyze image content to guide strategy selection
    ImageContent content = analyze_content(img);
    size_t raw_pixels = img.pixels.size();

    // Auto-scale: reduce effort for large images
    auto strategies = get_strategies(opts.level);
    bool is_small  = (raw_pixels < 8192);
    bool is_large  = (raw_pixels >= 262144);
    bool is_huge   = (raw_pixels >= 1048576);

    // Filter strategies based on content
    std::vector<Strategy> filtered;

    for (auto& s : strategies) {
        // Skip GA strategies for large images (too slow)
        if ((is_large || is_huge) && s.filter_level >= 5) continue;
        if (is_huge && s.filter_level >= 3) continue;

        // Skip palette-sort for non-indexed photo images
        if (s.palette_sort && content.is_photographic && img.color_type != 3)
            continue;

        // Skip alpha-zero off variants for non-transparent images
        if (!s.alpha_zero && img.color_type != 6 && img.color_type != 4)
            continue;

        // Skip high-iteration strategies for large images
        if (s.deflate_iterations >= 3 && is_large) continue;

        // For noise-like images, skip expensive strategies (nothing helps much)
        if (content.entropy_r > 7.5 && s.filter_level >= 3) continue;

        filtered.push_back(s);
    }

    // Ensure we have at least 2 strategies
    if (filtered.size() < 2) {
        filtered.push_back({2, CompressionLevel::Best, 1, true, false, false, "fallback"});
    }

    // Sort: try cheapest strategies first (so we can early-terminate)
    // Prioritize: lower filter_level, lower deflate_level, fewer iterations
    std::sort(filtered.begin(), filtered.end(),
        [](const Strategy& a, const Strategy& b) {
            int cost_a = a.filter_level * 100 + static_cast<int>(a.deflate_level) * 10 + a.deflate_iterations;
            int cost_b = b.filter_level * 100 + static_cast<int>(b.deflate_level) * 10 + b.deflate_iterations;
            return cost_a < cost_b;
        });

    if (opts.verbose) {
        std::cout << "Content: " << (content.is_photographic ? "photo" : "graphic")
                  << " gradient=" << content.gradient_strength
                  << " entropy_r=" << content.entropy_r
                  << " unique=" << content.unique_colors << "\n";
        std::cout << "Trying " << filtered.size() << " of " << strategies.size()
                  << " strategies (auto-scaled for " << raw_pixels << "px)\n";
    }

    // Run strategies with size tracking for early termination
    int threads = opts.num_threads;
    if (threads <= 0) threads = std::max(1u, std::thread::hardware_concurrency());
    if (is_large) threads = std::min(threads, 4); // don't thrash on large images

    struct TrialResult {
        std::vector<uint8_t> data;
        Strategy strategy;
        size_t size;
    };

    std::vector<TrialResult> results;
    results.reserve(filtered.size());
    size_t best_size_so_far = std::numeric_limits<size_t>::max();
    size_t best_strategy_idx = 0;

    // Two-tier: fast proxy first, then re-compress top 3 with best settings
    bool use_two_tier = (filtered.size() > 4) && !is_small;

    for (size_t si = 0; si < filtered.size(); ++si) {
        auto& s = filtered[si];

        // Early skip: if this strategy is much more expensive than best so far
        if (si > 2 && best_size_so_far < std::numeric_limits<size_t>::max()) {
            int cost_diff = (s.filter_level - filtered[best_strategy_idx].filter_level) * 100;
            if (cost_diff > 200) continue; // skip much more expensive strategies
        }

        Image work = img;
        if (s.alpha_zero && (img.color_type == 6 || img.color_type == 4))
            alpha_optimize(work);
        if (s.palette_sort)
            sort_palette(work);

        FilterOptions fopts;
        fopts.level = s.filter_level;
        fopts.window_size = std::min(3, s.filter_level);
        fopts.ga_population = std::min(s.filter_level * 5, 20); // scale GA down
        fopts.ga_generations = std::min(s.filter_level * 5, 30);
        auto filters = optimize_filters(work, fopts);

        size_t raw_ss = work.raw_scanline_size();
        size_t bpp = work.bytes_per_pixel();
        std::vector<uint8_t> filtered_data;
        filtered_data.reserve((raw_ss + 1) * work.height);
        std::vector<uint8_t> prev(raw_ss, 0);

        for (size_t y = 0; y < work.height; ++y) {
            const uint8_t* src = work.pixels.data() + y * raw_ss;
            std::vector<uint8_t> row(raw_ss + 1);
            FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
            filter_scanline(ft, src, row.data(), bpp, raw_ss,
                             y > 0 ? prev.data() : nullptr);
            filtered_data.insert(filtered_data.end(), row.begin(), row.end());
            std::memcpy(prev.data(), src, raw_ss);
        }

        // Use fast deflate for proxy trials (faster)
        DeflateOptions dopts;
        dopts.level = use_two_tier ? CompressionLevel::Fast : s.deflate_level;
        dopts.iterations = use_two_tier ? 1 : s.deflate_iterations;
        dopts.optimal_parsing = false;

        auto compressed = zlib_compress(filtered_data, dopts);

        TrialResult tr;
        tr.data = std::move(compressed);
        tr.strategy = s;
        tr.size = tr.data.size();
        results.push_back(std::move(tr));

        if (opts.verbose)
            std::cout << "  " << s.name << ": " << tr.size << " bytes\n";

        if (tr.size < best_size_so_far) {
            best_size_so_far = tr.size;
            best_strategy_idx = results.size() - 1;
        }
    }

    // Find best
    auto best = std::min_element(results.begin(), results.end(),
        [](const TrialResult& a, const TrialResult& b) { return a.size < b.size; });
    Strategy best_strat = best->strategy;

    if (opts.verbose)
        std::cout << "Best proxy: " << best_strat.name << " (" << best->size << " bytes)\n";

    // Two-tier: re-compress the winner with full settings if we used proxy
    if (use_two_tier) {
        // Re-do with best strategy at full quality
        Image work = img;
        if (best_strat.alpha_zero && (img.color_type == 6 || img.color_type == 4))
            alpha_optimize(work);
        if (best_strat.palette_sort)
            sort_palette(work);

        FilterOptions fopts;
        fopts.level = best_strat.filter_level;
        fopts.window_size = std::min(3, best_strat.filter_level);
        auto filters = optimize_filters(work, fopts);

        DeflateOptions dopts;
        dopts.level = best_strat.deflate_level;
        dopts.iterations = best_strat.deflate_iterations;
        dopts.optimal_parsing = (best_strat.deflate_iterations > 1);

        WriteOptions wopts;
        wopts.filters = filters;
        wopts.deflate = dopts;

        PNGWriter writer;
        auto png_out = writer.write(work, wopts);

        CompressResult result;
        result.data = std::move(png_out);
        result.time_seconds = total.elapsed_seconds();
        result.original_size = img.pixels.size();
        result.strategy_name = best_strat.name + "-full";
        return result;
    }

    // Single-tier: final output
    WriteOptions wopts;
    wopts.filters = {}; // auto-compute
    wopts.deflate.level = best_strat.deflate_level;
    wopts.deflate.iterations = best_strat.deflate_iterations;
    wopts.deflate.optimal_parsing = (best_strat.deflate_iterations > 1);
    wopts.deflate.bt_match_finder = best_strat.bt_match;

    // Need to re-filter since the writer auto-computes
    Image work2 = img;
    if (best_strat.alpha_zero && (img.color_type == 6 || img.color_type == 4))
        alpha_optimize(work2);
    if (best_strat.palette_sort)
        sort_palette(work2);

    // Recompute filters for the writer
    FilterOptions fopts;
    fopts.level = best_strat.filter_level;
    fopts.window_size = std::min(3, best_strat.filter_level);
    auto filters2 = optimize_filters(work2, fopts);
    wopts.filters = filters2;

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

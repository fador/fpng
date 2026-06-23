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
#include <future>
#include <mutex>
#include <atomic>

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

    // Maximum: try many strategies including GA filter optimization
    strategies.push_back({2, CompressionLevel::Default, 1, true, "max-01"});
    strategies.push_back({2, CompressionLevel::Best, 1, true, "max-02"});
    strategies.push_back({2, CompressionLevel::Best, 2, true, "max-03"});
    strategies.push_back({2, CompressionLevel::Ultra, 2, true, "max-04"});
    strategies.push_back({3, CompressionLevel::Best, 1, true, "max-05"});
    strategies.push_back({3, CompressionLevel::Best, 2, true, "max-06"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, "max-07"});

    // Alpha-zero off variants
    strategies.push_back({2, CompressionLevel::Best, 2, false, false, "max-08"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, false, false, "max-09"});

    // Palette-sort variants
    strategies.push_back({2, CompressionLevel::Best, 2, true, true, "max-10"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, true, "max-11"});

    // GA filter optimization (high-effort)
    strategies.push_back({5, CompressionLevel::Best, 2, true, "max-12"});
    strategies.push_back({7, CompressionLevel::Best, 2, true, "max-13"});
    strategies.push_back({7, CompressionLevel::Ultra, 3, true, "max-14"});

    // BT match finder variants (exhaustive matching)
    strategies.push_back({2, CompressionLevel::Best, 2, true, false, "max-15"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false, "max-16"});

    // Zopfli-style: multi-iteration refinement (4-5 passes)
    // Each pass rebuilds Huffman and re-parses with refined costs
    strategies.push_back({2, CompressionLevel::Ultra, 4, true, "max-17"});
    strategies.push_back({3, CompressionLevel::Ultra, 4, true, "max-18"});
    strategies.push_back({2, CompressionLevel::Ultra, 5, true, "max-19"});
    strategies.push_back({3, CompressionLevel::Ultra, 5, true, "max-20"});

    // Combined: BT match finder + high iterations + high filter
    strategies.push_back({2, CompressionLevel::Ultra, 4, true, false, "max-21"});
    strategies.push_back({3, CompressionLevel::Ultra, 5, true, false, "max-22"});

    // Pure quality: GA filter + BT match + max iterations
    strategies.push_back({7, CompressionLevel::Ultra, 5, true, false, "max-23"});

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
    bool is_small  = (raw_pixels < 16384);   // < 16KB raw
    bool is_large  = (raw_pixels >= 262144);
    bool is_huge   = (raw_pixels >= 1048576);

    // Filter strategies based on content
    std::vector<Strategy> filtered;

    for (auto& s : strategies) {
        // Skip GA strategies for large images (too slow)
        if ((is_large || is_huge) && s.filter_level >= 5) continue;
        if (is_huge && s.filter_level >= 3) continue;

        // Skip expensive strategies for very small images (won't help)
        if (is_small && s.filter_level >= 5) continue;
        if (is_small && s.deflate_iterations >= 4) continue;

        // Skip BT match finder strategies (correctness issues, WIP)

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
        filtered.push_back({2, CompressionLevel::Best, 1, true, false, "fallback"});
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

    // Run strategies in parallel batches
    int threads = opts.num_threads;
    if (threads <= 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min(threads, static_cast<int>(filtered.size()));

    // Atomic best size for early pruning across threads
    std::atomic<size_t> best_atomic{std::numeric_limits<size_t>::max()};

    struct TrialResult {
        std::vector<uint8_t> data;
        Strategy strategy;
        size_t size;
    };

    std::vector<TrialResult> results;
    results.reserve(filtered.size());

    auto run_one_trial = [&](const Strategy& s) -> TrialResult {
        // Early prune: if another thread found much better, skip expensive strategies
        size_t current_best = best_atomic.load();
        if (current_best < std::numeric_limits<size_t>::max() && s.filter_level >= 5) {
            return {{}, s, std::numeric_limits<size_t>::max()};
        }

        Image work = img;
        if (s.alpha_zero && (img.color_type == 6 || img.color_type == 4))
            alpha_optimize(work);
        if (s.palette_sort)
            sort_palette(work);

        FilterOptions fopts;
        fopts.level = s.filter_level;
        fopts.window_size = std::min(3, s.filter_level);
        fopts.ga_population = std::min(s.filter_level * 5, 20);
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

        DeflateOptions dopts;
        dopts.level = CompressionLevel::Fast;
        dopts.iterations = 1;
        dopts.optimal_parsing = false;
        if (s.deflate_level >= CompressionLevel::Ultra)
            dopts.chain_depth = 256;

        TrialResult tr;
        tr.data = zlib_compress(filtered_data, dopts);
        tr.strategy = s;
        tr.size = tr.data.size();

        // Update atomic best (smaller is better)
        size_t prev_best = best_atomic.load();
        while (tr.size < prev_best && !best_atomic.compare_exchange_weak(prev_best, tr.size)) {}

        return tr;
    };

    // Process strategies in batches to limit concurrency
    for (size_t batch_start = 0; batch_start < filtered.size(); batch_start += threads) {
        size_t batch_end = std::min(batch_start + static_cast<size_t>(threads), filtered.size());
        std::vector<std::future<TrialResult>> batch_futures;

        for (size_t si = batch_start; si < batch_end; ++si) {
            batch_futures.push_back(std::async(std::launch::async, run_one_trial,
                                                std::ref(filtered[si])));
        }

        // Collect batch results
        for (auto& f : batch_futures) {
            auto tr = f.get();
            if (opts.verbose)
                std::cout << "  " << tr.strategy.name << ": " << tr.size << " bytes\n";
            if (tr.size < std::numeric_limits<size_t>::max())
                results.push_back(std::move(tr));
        }
    }

    // Find best
    auto best = std::min_element(results.begin(), results.end(),
        [](const TrialResult& a, const TrialResult& b) { return a.size < b.size; });
    Strategy best_strat = best->strategy;

    if (opts.verbose)
        std::cout << "Best proxy: " << best_strat.name << " (" << best->size << " bytes)\n";

    // Two-tier: re-compress top 3 proxy winners with full settings
    // Always do this to fix proxy/actual compression mismatch
    if (filtered.size() > 0) {
        auto sorted = results;
        std::sort(sorted.begin(), sorted.end(),
            [](const TrialResult& a, const TrialResult& b) { return a.size < b.size; });

        size_t best_final = std::numeric_limits<size_t>::max();
        CompressResult best_result;

        for (size_t ri = 0; ri < std::min(sorted.size(), size_t(3)); ++ri) {
            auto& strat = sorted[ri].strategy;

            Image work = img;
            if (strat.alpha_zero && (img.color_type == 6 || img.color_type == 4))
                alpha_optimize(work);
            if (strat.palette_sort) sort_palette(work);

            FilterOptions fopts;
            fopts.level = strat.filter_level;
            fopts.window_size = std::min(3, strat.filter_level);
            auto filters = optimize_filters(work, fopts);

            DeflateOptions dopts;
            dopts.level = strat.deflate_level;
            dopts.iterations = strat.deflate_iterations;
            dopts.optimal_parsing = (strat.deflate_iterations > 1);
                dopts.chain_depth = 512;

            WriteOptions wopts;
            wopts.filters = filters;
            wopts.deflate = dopts;

            PNGWriter writer;
            auto png_out = writer.write(work, wopts);

            if (opts.verbose)
                std::cout << "  Actual " << strat.name << ": " << png_out.size() << " bytes PNG\n";

            if (png_out.size() < best_final || ri == 0) {
                best_final = png_out.size();
                best_result.data = std::move(png_out);
                best_result.strategy_name = strat.name + "-actual";
            }
        }

        best_result.time_seconds = total.elapsed_seconds();
        best_result.original_size = img.pixels.size();
        return best_result;
    }

    // Single-tier: final output
    WriteOptions wopts;
    wopts.filters = {}; // auto-compute
    wopts.deflate.level = best_strat.deflate_level;
    wopts.deflate.iterations = best_strat.deflate_iterations;
    wopts.deflate.optimal_parsing = (best_strat.deflate_iterations > 1);

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

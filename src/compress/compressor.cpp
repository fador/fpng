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
    strategies.push_back({2, CompressionLevel::Default, 1, true, false, "max-01"});
    strategies.push_back({2, CompressionLevel::Best, 1, true, false, "max-02"});
    strategies.push_back({2, CompressionLevel::Best, 2, true, false, "max-03"});
    strategies.push_back({2, CompressionLevel::Ultra, 2, true, false, "max-04"});
    strategies.push_back({3, CompressionLevel::Best, 1, true, false, "max-05"});
    strategies.push_back({3, CompressionLevel::Best, 2, true, false, "max-06"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false, "max-07"});

    // Alpha-zero off variants
    strategies.push_back({2, CompressionLevel::Best, 2, false, false, "max-08"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, false, false, "max-09"});

    // Palette-sort variants
    strategies.push_back({2, CompressionLevel::Best, 2, true, true, "max-10"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, true, "max-11"});

    // GA filter optimization (high-effort)
    strategies.push_back({5, CompressionLevel::Best, 2, true, false, "max-12"});
    strategies.push_back({7, CompressionLevel::Best, 2, true, false, "max-13"});
    strategies.push_back({7, CompressionLevel::Ultra, 3, true, false, "max-14"});

    // BT match finder variants (exhaustive matching)
    strategies.push_back({2, CompressionLevel::Best, 2, true, false, "max-15"});
    strategies.push_back({3, CompressionLevel::Ultra, 3, true, false, "max-16"});

    // Zopfli-style: multi-iteration refinement (4-5 passes)
    // Each pass rebuilds Huffman and re-parses with refined costs
    strategies.push_back({2, CompressionLevel::Ultra, 4, true, false, "max-17"});
    strategies.push_back({3, CompressionLevel::Ultra, 4, true, false, "max-18"});
    strategies.push_back({2, CompressionLevel::Ultra, 5, true, false, "max-19"});
    strategies.push_back({3, CompressionLevel::Ultra, 5, true, false, "max-20"});

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
    dopts.adaptive_blocks = false;

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
    bool is_large  = (raw_pixels >= 262144);
    bool is_huge   = (raw_pixels >= 1048576);

    // Filter strategies based on content and size
    std::vector<Strategy> filtered;
    int max_filtered = is_huge ? 3 : (is_large ? 7 : (int)strategies.size());
    bool ga_allowed = false; // allow exactly 1 GA strategy for large images

    for (auto& s : strategies) {
        // Skip GA strategies for huge images, allow 1 for large
        if (is_huge && s.filter_level >= 5) continue;
        if (is_large && s.filter_level >= 5) {
            if (!ga_allowed) { ga_allowed = true; /* keep this one */ }
            else continue;
        }
        if (is_huge && s.filter_level >= 3) continue;

        // Skip high-iteration strategies for large images
        if (s.deflate_iterations >= 3 && (is_large || is_huge)) continue;

        // For noise-like images, skip expensive strategies (nothing helps much)
        if (content.entropy_r > 7.5 && s.filter_level >= 3) continue;

        filtered.push_back(s);
    }

    // Ensure we have at least 2 strategies
    if (filtered.size() < 2) {
        filtered.push_back({2, CompressionLevel::Best, 1, true, false, "fallback"});
    }

    // Limit to max_filtered
    if ((int)filtered.size() > max_filtered)
        filtered.resize(max_filtered);

    // Force-include one GA strategy for large images (it was sorted to the end
    // by cost and would be cut by max_filtered, but filter quality matters).
    if (is_large && !is_huge) {
        bool has_ga = false;
        for (auto& f : filtered) {
            if (f.filter_level >= 5) { has_ga = true; break; }
        }
        if (!has_ga) {
            // Find the first GA strategy from the original list
            for (auto& s : strategies) {
                if (s.filter_level >= 5) {
                    filtered.push_back(s);
                    break;
                }
            }
        }
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
        // Reduce GA effort for large images, disable entirely for huge
        if (is_huge) {
            fopts.ga_population = 0;
            fopts.ga_generations = 0;
        } else if (is_large) {
            fopts.ga_population = std::min(s.filter_level, 5);
            fopts.ga_generations = std::min(s.filter_level, 10);
        }
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
        dopts.level = CompressionLevel::Default; // better proxy fidelity than Fast
        dopts.iterations = 1;
        dopts.optimal_parsing = false;
        dopts.adaptive_blocks = !is_huge; // adaptive for all but huge images
        dopts.chain_depth = 0; // auto-select based on level
        dopts.max_block_size = 65536; // large blocks for proxy (no tree overhead with Fixed)

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

    // Two-tier: re-compress top N proxy winners with full settings
    // Always do this to fix proxy/actual compression mismatch
    if (filtered.size() > 0) {
        auto sorted = results;
        std::sort(sorted.begin(), sorted.end(),
            [](const TrialResult& a, const TrialResult& b) { return a.size < b.size; });

        size_t best_final = std::numeric_limits<size_t>::max();
        CompressResult best_result;

        // For huge images, only re-compress the single best proxy to save time
        int recompress_count = is_huge ? 1 : (is_large ? 2 : 3);
        // For medium images, also clamp at 2 to keep things snappy
        if (!is_large && !is_huge) recompress_count = std::min(recompress_count, 2);

        // Force-include GA strategy in re-compress for large images
        // (its Fixed-Huffman proxy rank underrates it — Dynamic Huffman
        //  benefits much more from GA-chosen filters)
        size_t ga_idx = std::numeric_limits<size_t>::max();
        if (is_large && !is_huge) {
            for (size_t si = 0; si < sorted.size(); ++si) {
                if (sorted[si].strategy.filter_level >= 5) {
                    ga_idx = si;
                    break;
                }
            }
            if (ga_idx != std::numeric_limits<size_t>::max() && ga_idx >= (size_t)recompress_count) {
                // Swap GA strategy into position 1 for re-compress (keep best at position 0)
                std::swap(sorted[1], sorted[ga_idx]);
            }
        }

        for (size_t ri = 0; ri < std::min(sorted.size(), size_t(recompress_count)); ++ri) {
            auto& strat = sorted[ri].strategy;

            Image work = img;
            if (strat.alpha_zero && (img.color_type == 6 || img.color_type == 4))
                alpha_optimize(work);
            if (strat.palette_sort) sort_palette(work);

            FilterOptions fopts;
            fopts.level = strat.filter_level;
            fopts.window_size = std::min(3, strat.filter_level);
            // Enable GA for large images (small population) to get better filters
            if (is_huge) {
                fopts.ga_population = 0; fopts.ga_generations = 0;
            } else if (is_large) {
                fopts.ga_population = std::min(strat.filter_level, 5);
                fopts.ga_generations = std::min(strat.filter_level, 10);
            } else {
                fopts.ga_population = std::min(strat.filter_level * 5, 20);
                fopts.ga_generations = std::min(strat.filter_level * 5, 30);
            }
            auto filters = optimize_filters(work, fopts);

            DeflateOptions dopts;
            dopts.level = strat.deflate_level;
            dopts.iterations = strat.deflate_iterations;
            dopts.adaptive_blocks = !is_huge; // adaptive for all but huge
            if (is_huge) {
                dopts.iterations = std::min(dopts.iterations, 1);
                dopts.level = CompressionLevel::Best;
            } else if (is_large) {
                // Bump to at least 3 iterations for Huffman cost feedback on iter 3
                dopts.iterations = std::max(std::min(dopts.iterations, 3), 3);
            }
            dopts.optimal_parsing = (dopts.iterations > 1);
            dopts.chain_depth = 0; // auto-select based on level

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

        // === Strategy hybrids: cross-pollinate top-2 proxy winners ===
        // Mix parameters from the two best strategies to discover better
        // combinations not in the original strategy list.
        if (!is_huge && sorted.size() >= 2) {
            auto& s1 = sorted[0].strategy;

            // Find a second strategy that differs from s1 in at least one parameter
            Strategy s2 = sorted[1].strategy;
            for (size_t si = 1; si < sorted.size(); ++si) {
                auto& candidate = sorted[si].strategy;
                if (candidate.filter_level != s1.filter_level ||
                    candidate.deflate_level != s1.deflate_level ||
                    candidate.deflate_iterations != s1.deflate_iterations ||
                    candidate.alpha_zero != s1.alpha_zero ||
                    candidate.palette_sort != s1.palette_sort) {
                    s2 = candidate;
                    break;
                }
            }

            // Generate hybrids: mix filter_level, deflate_level, iterations,
            // alpha_zero, and palette_sort from the two parents.
            // Each hybrid gets one parameter from s2, rest from s1.
            struct Variant { int fl; CompressionLevel dl; int it; bool az; bool ps; };
            std::vector<Variant> hybrids;

            // Only vary parameters that actually differ
            if (s2.filter_level != s1.filter_level) {
                hybrids.push_back({s2.filter_level, s1.deflate_level, s1.deflate_iterations,
                                   s1.alpha_zero, s1.palette_sort});
            }
            if (s2.deflate_level != s1.deflate_level) {
                hybrids.push_back({s1.filter_level, s2.deflate_level, s1.deflate_iterations,
                                   s1.alpha_zero, s1.palette_sort});
            }
            if (s2.deflate_iterations != s1.deflate_iterations) {
                hybrids.push_back({s1.filter_level, s1.deflate_level, s2.deflate_iterations,
                                   s1.alpha_zero, s1.palette_sort});
            }
            // Vary alpha_zero only if image actually has alpha
            if (s2.alpha_zero != s1.alpha_zero &&
                (img.color_type == 6 || img.color_type == 4)) {
                hybrids.push_back({s1.filter_level, s1.deflate_level, s1.deflate_iterations,
                                   s2.alpha_zero, s1.palette_sort});
            }
            if (s2.palette_sort != s1.palette_sort) {
                hybrids.push_back({s1.filter_level, s1.deflate_level, s1.deflate_iterations,
                                   s1.alpha_zero, s2.palette_sort});
            }

            // Try each hybrid (skip if identical to an already-tested strategy)
            for (auto& h : hybrids) {
                // Skip if same as s1 or s2 (already tested)
                if (h.fl == s1.filter_level && h.dl == s1.deflate_level &&
                    h.it == s1.deflate_iterations && h.az == s1.alpha_zero &&
                    h.ps == s1.palette_sort) continue;
                if (h.fl == s2.filter_level && h.dl == s2.deflate_level &&
                    h.it == s2.deflate_iterations && h.az == s2.alpha_zero &&
                    h.ps == s2.palette_sort) continue;

                // Allow up to 2 parameter changes (not just 1)
                int changes = 0;
                if (h.fl != s1.filter_level) ++changes;
                if (h.dl != s1.deflate_level) ++changes;
                if (h.it != s1.deflate_iterations) ++changes;
                if (h.az != s1.alpha_zero) ++changes;
                if (h.ps != s1.palette_sort) ++changes;
                if (changes > 2) continue;

                Image work = img;
                if (h.az && (img.color_type == 6 || img.color_type == 4))
                    alpha_optimize(work);
                if (h.ps) sort_palette(work);

                FilterOptions fopts;
                fopts.level = h.fl;
                fopts.window_size = std::min(3, h.fl);
                fopts.ga_population = 5; // minimal GA for hybrid
                fopts.ga_generations = 10;
                auto f = optimize_filters(work, fopts);

                DeflateOptions d;
                d.level = h.dl;
                d.iterations = is_huge ? 1 : h.it;
                d.optimal_parsing = (d.iterations > 1);
                d.adaptive_blocks = !is_huge && !is_large;
                d.chain_depth = 0;

                WriteOptions w;
                w.filters = f;
                w.deflate = d;

                PNGWriter wr;
                auto png = wr.write(work, w);

                if (opts.verbose) {
                    std::string hybrid_name = std::string("hybrid-fl") +
                        std::to_string(h.fl) + "-dl" + std::to_string((int)h.dl) +
                        "-it" + std::to_string(h.it) +
                        (h.az ? "-az" : "") + (h.ps ? "-ps" : "");
                    std::cout << "  Hybrid " << hybrid_name << ": "
                              << png.size() << " bytes PNG\n";
                }

                if (png.size() < best_final) {
                    best_final = png.size();
                    best_result.data = std::move(png);
                    best_result.strategy_name = "hybrid";
                }
            }
        }

        // === Uniform filter polish ===
        // Per-row filter selection (MinSum/entropy/GA) can miss globally-optimal
        // uniform filters because per-row fitness doesn't account for cross-row
        // pattern consistency that LZ77 exploits. Try all 5 uniform filter sets
        // with the best strategy's parameters as a refinement pass.
        if (!is_huge) {
            // Use best-result-winning parameters but try uniform filter sets
            // with both alpha_zero on and off for images that have alpha.
            Strategy ref = sorted[0].strategy;
            FilterType uniform_types[] = {
                FilterType::None, FilterType::Sub, FilterType::Up,
                FilterType::Average, FilterType::Paeth
            };
            bool alphas_tried[2] = {ref.alpha_zero, !ref.alpha_zero};
            int alpha_count = (img.color_type == 6 || img.color_type == 4) ? 2 : 1;

            for (int ai = 0; ai < alpha_count; ++ai) {
                bool use_alpha = alphas_tried[ai];
                // Skip duplicate (both alphas_tried entries are the same for non-alpha images)
                if (ai > 0 && use_alpha == alphas_tried[0]) break;

                for (auto ft : uniform_types) {
                    Image work = img;
                    if (use_alpha && (img.color_type == 6 || img.color_type == 4))
                        alpha_optimize(work);
                    if (ref.palette_sort) sort_palette(work);

                std::vector<FilterType> uniforms(work.height, ft);

                DeflateOptions d;
                d.level = ref.deflate_level;
                d.iterations = is_huge ? 1 :
                    (is_large ? std::min(ref.deflate_iterations, 2) : ref.deflate_iterations);
                d.optimal_parsing = (d.iterations > 1);
                d.adaptive_blocks = !is_huge && !is_large;
                d.chain_depth = 0;

                WriteOptions w;
                w.filters = uniforms;
                w.deflate = d;

                PNGWriter wr;
                auto png = wr.write(work, w);

                if (opts.verbose) {
                    const char* fname = ft == FilterType::None ? "None" :
                        ft == FilterType::Sub ? "Sub" :
                        ft == FilterType::Up ? "Up" :
                        ft == FilterType::Average ? "Avg" : "Paeth";
                    std::cout << "  Uniform " << fname << ": "
                              << png.size() << " bytes PNG\n";
                }

                if (png.size() < best_final) {
                    best_final = png.size();
                    best_result.data = std::move(png);
                    best_result.strategy_name = "uniform-filter";
                }
            }
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

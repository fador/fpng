#include "compress/compressor.hpp"
#include "compress/deflate/deflater.hpp"
#include "compress/filter_optimizer.hpp"
#include "png/writer.hpp"
#include "png/reader.hpp"
#include "preprocess/preprocessor.hpp"
#include "preprocess/alpha_optimizer.hpp"
#include "preprocess/palette_sorter.hpp"
#include "preprocess/color_reducer.hpp"
#include "preprocess/content_analyzer.hpp"
#include "util/timer.hpp"
#include "image/compare.hpp"

#include <stdexcept>

#include <algorithm>
#include <thread>
#include <iostream>
#include <cstring>
#include <future>
#include <mutex>
#include <atomic>

namespace fpng {

namespace {

void apply_strip_ancillary(Image& img, bool strip_ancillary, bool keep_color_info) {
    if (!strip_ancillary) return;
    img.ancillary.bKGD.reset();
    img.ancillary.pHYs.reset();
    img.ancillary.tIME.reset();
    img.ancillary.sBIT.reset();
    img.ancillary.text_chunks.clear();
    img.ancillary.other_chunks.clear();
    if (!keep_color_info) {
        img.ancillary.gAMA.reset();
        img.ancillary.cHRM.reset();
        img.ancillary.sRGB.reset();
        img.ancillary.iCCP.reset();
        img.ancillary.cICP.reset();
    }
}

size_t extra_chunk_bytes(const Image& work) {
    size_t extra = 0;
    if (!work.palette.empty()) {
        extra += 12 + work.palette.size();
    }
    if (work.color_type == 3) {
        if (!work.alpha_palette.empty()) {
            size_t n = work.alpha_palette.size();
            while (n > 0 && work.alpha_palette[n - 1] == 255) --n;
            if (n > 0) extra += 12 + n;
        }
    } else if (!work.ancillary.tRNS.empty()) {
        extra += 12 + work.ancillary.tRNS.size();
    }
    return extra;
}

bool same_preprocessed_image(const Image& a, const Image& b) {
    return a.color_type == b.color_type &&
           a.bit_depth == b.bit_depth &&
           a.pixels == b.pixels &&
           a.palette == b.palette &&
           a.alpha_palette == b.alpha_palette &&
           a.ancillary.tRNS == b.ancillary.tRNS;
}

std::vector<uint8_t> build_filtered_stream(const Image& work,
                                           const std::vector<FilterType>& filters) {
    size_t raw_ss = work.raw_scanline_size();
    size_t bpp = work.bytes_per_pixel();
    std::vector<uint8_t> filtered_data;
    filtered_data.reserve((raw_ss + 1) * work.height);
    std::vector<uint8_t> prev(raw_ss, 0);
    std::vector<uint8_t> row(raw_ss + 1);

    for (size_t y = 0; y < work.height; ++y) {
        const uint8_t* src = work.pixels.data() + y * raw_ss;
        FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
        filter_scanline(ft, src, row.data(), bpp, raw_ss,
                         y > 0 ? prev.data() : nullptr);
        filtered_data.insert(filtered_data.end(), row.begin(), row.end());
        std::memcpy(prev.data(), src, raw_ss);
    }
    return filtered_data;
}

// Run fn(i) for i in [0, n) on up to `threads` workers that pull indices from
// a shared counter. Unlike fixed batches of `threads` futures, a worker that
// finishes early immediately takes the next job, so one slow job no longer
// idles the other cores until the whole batch completes. Results must be
// written by index, so the output does not depend on scheduling.
template <class Fn>
void parallel_for(size_t n, int threads, Fn&& fn) {
    if (n == 0) return;
    size_t workers = std::min(n, static_cast<size_t>(std::max(1, threads)));
    if (workers == 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (size_t t = 1; t < workers; ++t) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
}

} // namespace

std::vector<Strategy> get_strategies(int level) {
    std::vector<Strategy> strategies;

    if (level <= 0) {
        // Fast: just one strategy
        strategies.push_back({0, CompressionLevel::Fast, 1, true, false, "fast"});
        return strategies;
    }

    if (level <= 3) {
        // Balanced: distinct filter heuristics
        strategies.push_back({0, CompressionLevel::Default, 1, true, false, "balanced-minsum"});
        strategies.push_back({1, CompressionLevel::Best, 1, true, false, "balanced-cument"});
        strategies.push_back({2, CompressionLevel::Best, 1, true, false, "balanced-ent"});
        return strategies;
    }

    if (level <= 6) {
        // Good: keep color_reduce=false at level <= 6 so RGB/RGBA layout is preserved
        strategies.push_back({0, CompressionLevel::Best, 2, true, false, "good-minsum"});
        strategies.push_back({1, CompressionLevel::Best, 2, true, false, "good-cument"});
        strategies.push_back({2, CompressionLevel::Best, 2, true, false, "good-ent"});
        strategies.push_back({3, CompressionLevel::Best, 2, true, false, "good-trial"});
        strategies.push_back({5, CompressionLevel::Best, 2, true, false, "good-viterbi"});
        strategies.push_back({2, CompressionLevel::Best, 2, true, true,  "good-ps"});
        return strategies;
    }

    int target_iters = (level >= 9) ? 5 : 3;

    // Maximum: distinct (filter_level, alpha_zero, palette_sort, color_reduce) strategies
    // 1. Standard preprocessing (alpha_zero = true) across all filter levels
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, true, false, "max-fl0-az", false});
    strategies.push_back({1, CompressionLevel::Ultra, target_iters, true, false, "max-fl1-az", false});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, true, false, "max-fl2-az", false});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, true, false, "max-fl3-az", false});
    strategies.push_back({4, CompressionLevel::Ultra, target_iters, true, false, "max-fl4-az", false});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, true, false, "max-fl5-az", false});
    strategies.push_back({7, CompressionLevel::Ultra, target_iters, true, false, "max-fl7-az", false});

    // 2. Alpha-zero off variants
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, false, false, "max-fl0-noaz", false});
    strategies.push_back({1, CompressionLevel::Ultra, target_iters, false, false, "max-fl1-noaz", false});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, false, false, "max-fl2-noaz", false});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, false, false, "max-fl3-noaz", false});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, false, false, "max-fl5-noaz", false});
    strategies.push_back({7, CompressionLevel::Ultra, target_iters, false, false, "max-fl7-noaz", false});

    // 3. Palette-sort variants (for already-indexed images)
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, true, true, "max-fl0-ps", false});
    strategies.push_back({1, CompressionLevel::Ultra, target_iters, true, true, "max-fl1-ps", false});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, true, true, "max-fl2-ps", false});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, true, true, "max-fl3-ps", false});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, true, true, "max-fl5-ps", false});

    // 4. Color-reduction variants (RGB->gray, truecolor->indexed, sub-byte packing, 16->8 bit)
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, true, false, "max-fl0-cr", true});
    strategies.push_back({1, CompressionLevel::Ultra, target_iters, true, false, "max-fl1-cr", true});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, true, false, "max-fl2-cr", true});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, true, false, "max-fl3-cr", true});
    strategies.push_back({4, CompressionLevel::Ultra, target_iters, true, false, "max-fl4-cr", true});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, true, false, "max-fl5-cr", true});
    strategies.push_back({7, CompressionLevel::Ultra, target_iters, true, false, "max-fl7-cr", true});

    // 5. Color-reduction + Palette-sort variants
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, true, true, "max-fl0-cr-ps", true});
    strategies.push_back({1, CompressionLevel::Ultra, target_iters, true, true, "max-fl1-cr-ps", true});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, true, true, "max-fl2-cr-ps", true});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, true, true, "max-fl3-cr-ps", true});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, true, true, "max-fl5-cr-ps", true});
    strategies.push_back({7, CompressionLevel::Ultra, target_iters, true, true, "max-fl7-cr-ps", true});

    // 6. Color-reduction without alpha_zero (for RGBA -> Indexed with tRNS instead of RGB+tRNS)
    strategies.push_back({0, CompressionLevel::Ultra, target_iters, false, true, "max-fl0-noaz-cr-ps", true});
    strategies.push_back({2, CompressionLevel::Ultra, target_iters, false, true, "max-fl2-noaz-cr-ps", true});
    strategies.push_back({3, CompressionLevel::Ultra, target_iters, false, true, "max-fl3-noaz-cr-ps", true});
    strategies.push_back({5, CompressionLevel::Ultra, target_iters, false, true, "max-fl5-noaz-cr-ps", true});

    return strategies;
}

std::vector<uint8_t> run_strategy(const Image& img, const Strategy& s) {
    Image work = img;

    bool multi_frame = (img.is_animated && !img.frames.empty());
    if (!multi_frame) {
        if (s.alpha_zero) {
            alpha_optimize(work);
        }
        if (s.color_reduce) {
            reduce_colors(work);
        }
        if (s.palette_sort) {
            sort_palette(work);
        }
    }

    // Filter optimization
    FilterOptions fopts;
    fopts.level = s.filter_level;
    fopts.window_size = std::min(3, std::max(1, s.filter_level));
    auto filters = optimize_filters(work, fopts);

    auto filtered = build_filtered_stream(work, filters);

    // Deflate
    DeflateOptions dopts;
    dopts.level = s.deflate_level;
    dopts.iterations = s.deflate_iterations;
    dopts.adaptive_blocks = true;
    dopts.row_stride = static_cast<int>(work.raw_scanline_size() + 1);

    return zlib_compress(filtered, dopts);
}

CompressResult compress_single(const Image& img, const CompressOptions& opts) {
    Timer t;

    Image work = img;
    apply_strip_ancillary(work, opts.strip_ancillary, opts.keep_color_info);

    bool multi_frame = (work.is_animated && !work.frames.empty());
    if (!multi_frame) {
        if (work.color_type == 6 || work.color_type == 4 || work.color_type == 3)
            alpha_optimize(work);
        if (opts.level >= 7)
            reduce_colors(work);
        if (work.color_type == 3)
            sort_palette(work);
    }

    size_t raw_ss = work.raw_scanline_size();
    FilterOptions fopts;
    fopts.level = opts.level >= 7 ? 7 : (opts.level >= 5 ? 5 : (opts.level >= 3 ? 3 : 2));
    fopts.window_size = std::min(3, std::max(1, fopts.level));
    auto filters = optimize_filters(work, fopts);

    DeflateOptions dopts;
    dopts.level = opts.level >= 9 ? CompressionLevel::Ultra :
                  (opts.level >= 5 ? CompressionLevel::Best : CompressionLevel::Default);
    dopts.iterations = opts.level >= 9 ? 3 : (opts.level >= 5 ? 2 : 1);
    dopts.row_stride = static_cast<int>(raw_ss + 1);
    dopts.adaptive_blocks = true;

    WriteOptions wopts;
    wopts.filters = filters;
    wopts.deflate = dopts;

    PNGWriter writer;
    auto png_out = writer.write(work, wopts);
    std::string best_strat = "single-viterbi";

    if (opts.level >= 5) {
        FilterType candidates[] = {
            FilterType::None, FilterType::Sub, FilterType::Up,
            FilterType::Average, FilterType::Paeth
        };
        for (auto ft : candidates) {
            std::vector<FilterType> uniforms(work.height, ft);
            WriteOptions uwopts;
            uwopts.filters = uniforms;
            uwopts.deflate = dopts;
            auto trial_png = writer.write(work, uwopts);
            if (trial_png.size() < png_out.size()) {
                png_out = std::move(trial_png);
                best_strat = "single-uniform";
            }
        }
    }

    CompressResult result;
    result.data = std::move(png_out);
    result.time_seconds = t.elapsed_seconds();
    result.original_size = img.pixels.size();
    result.strategy_name = best_strat;
    return result;
}

CompressResult compress_multi(const Image& img_in, const CompressOptions& opts) {

    Timer total;

    Image img = img_in;
    apply_strip_ancillary(img, opts.strip_ancillary, opts.keep_color_info);

    // Analyze image content to guide strategy selection
    ImageContent content = analyze_content(img);
    size_t raw_pixels = img.pixels.size();

    auto strategies = get_strategies(opts.level);
    bool is_large  = (raw_pixels >= 262144);
    bool is_huge   = (raw_pixels >= 1048576);
    bool multi_frame = (img.is_animated && !img.frames.empty());

    // Filter strategies based on content and size
    std::vector<Strategy> filtered;
    for (auto& s : strategies) {
        if (multi_frame && (s.alpha_zero || s.color_reduce || s.palette_sort)) {
            if (s.color_reduce || s.palette_sort) continue;
        }
        // Skip level 4 (3-row windowed trial) and level 7 on large/huge images to bound filter time
        if ((is_large || is_huge) && (s.filter_level == 4 || s.filter_level == 7)) {
            continue;
        }
        // For noise-like images, skip expensive filter strategies
        if (content.entropy_r > 7.5 && s.filter_level >= 3) continue;

        filtered.push_back(s);
    }

    if (filtered.empty()) {
        filtered.push_back({2, CompressionLevel::Best, 1, !multi_frame, false, "fallback"});
    }

    // Step 1: Build unique pre-processed Image variants so identical pixel/palette
    // representations are never filtered or compressed more than once.
    struct PreprocessedVariant {
        Image work;
        bool alpha_zero;
        bool color_reduce;
        bool palette_sort;
        size_t chunk_overhead;
    };
    std::vector<PreprocessedVariant> variants;

    auto get_or_create_variant = [&](bool az, bool cr, bool ps) -> size_t {
        if (multi_frame) {
            az = false;
            cr = false;
            ps = false;
        }
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants[i].alpha_zero == az &&
                variants[i].color_reduce == cr &&
                variants[i].palette_sort == ps) {
                return i;
            }
        }
        Image work = img;
        if (az && (work.color_type == 6 || work.color_type == 4 || work.color_type == 3))
            alpha_optimize(work);
        if (cr)
            reduce_colors(work);
        if (ps && work.color_type == 3)
            sort_palette(work);

        // Deduplicate against existing variants by actual resulting Image content
        for (size_t i = 0; i < variants.size(); ++i) {
            if (same_preprocessed_image(variants[i].work, work)) {
                return i;
            }
        }

        size_t idx = variants.size();
        size_t overhead = extra_chunk_bytes(work);
        variants.push_back({std::move(work), az, cr, ps, overhead});
        return idx;
    };

    struct FilterJob {
        size_t variant_idx;
        Strategy strategy;
        bool is_uniform = false;
        FilterType uniform_type = FilterType::None;
    };
    std::vector<FilterJob> filter_jobs;

    // optimize_filters() output depends only on this key: levels 3 and 4 both
    // run the windowed trial with window_size = min(3, level) = 3, and 5/6
    // share the same Viterbi penalty. Jobs with the same key on the same
    // variant would produce identical filter vectors (later deduplicated as
    // candidates, keeping the first), so compute them once.
    auto filter_key = [](int level) {
        if (level == 4) return 3;
        if (level == 6) return 5;
        return level;
    };
    for (const auto& s : filtered) {
        size_t v_idx = get_or_create_variant(s.alpha_zero, s.color_reduce, s.palette_sort);
        bool exists = false;
        for (const auto& fj : filter_jobs) {
            if (!fj.is_uniform && fj.variant_idx == v_idx &&
                filter_key(fj.strategy.filter_level) == filter_key(s.filter_level)) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            filter_jobs.push_back({v_idx, s, false, FilterType::None});
        }
    }

    // Also add the 5 uniform filters for each unique pre-processed variant
    if (opts.level >= 4) {
        CompressionLevel def_lvl = (opts.level >= 7) ? CompressionLevel::Ultra : CompressionLevel::Best;
        int def_iters = (opts.level >= 9) ? 5 : ((opts.level >= 7) ? 3 : 2);
        FilterType uniform_types[] = {
            FilterType::None, FilterType::Sub, FilterType::Up,
            FilterType::Average, FilterType::Paeth
        };
        const char* uniform_names[] = {"unif-none", "unif-sub", "unif-up", "unif-avg", "unif-paeth"};
        for (size_t v_idx = 0; v_idx < variants.size(); ++v_idx) {
            for (int ui = 0; ui < 5; ++ui) {
                Strategy us;
                us.filter_level = 0;
                us.deflate_level = def_lvl;
                us.deflate_iterations = def_iters;
                us.alpha_zero = variants[v_idx].alpha_zero;
                us.palette_sort = variants[v_idx].palette_sort;
                us.color_reduce = variants[v_idx].color_reduce;
                us.name = std::string(uniform_names[ui]) + "-v" + std::to_string(v_idx);
                filter_jobs.push_back({v_idx, us, true, uniform_types[ui]});
            }
        }
    }

    int threads = opts.num_threads;
    if (threads <= 0) threads = std::max(1u, std::thread::hardware_concurrency());

    // Step 2: Compute filter vectors for all jobs in parallel
    std::vector<std::vector<FilterType>> job_filters(filter_jobs.size());
    parallel_for(filter_jobs.size(), threads, [&](size_t ji) {
        const auto& job = filter_jobs[ji];
        const Image& work = variants[job.variant_idx].work;
        if (job.is_uniform) {
            job_filters[ji] = std::vector<FilterType>(work.height, job.uniform_type);
            return;
        }
        FilterOptions fopts;
        fopts.level = job.strategy.filter_level;
        fopts.window_size = std::min(3, std::max(1, job.strategy.filter_level));
        if (is_huge || is_large) {
            fopts.ga_population = std::min(job.strategy.filter_level, 5);
            fopts.ga_generations = std::min(job.strategy.filter_level, 10);
        } else {
            fopts.ga_population = std::min(job.strategy.filter_level * 5, 20);
            fopts.ga_generations = std::min(job.strategy.filter_level * 5, 30);
        }
        job_filters[ji] = optimize_filters(work, fopts);
    });

    // Step 3: Deduplicate (variant_idx, filters) so identical filtered scanlines
    // are never proxy-compressed twice.
    struct UniqueCandidate {
        size_t variant_idx;
        std::vector<FilterType> filters;
        Strategy strategy;
    };
    std::vector<UniqueCandidate> candidates;
    for (size_t ji = 0; ji < filter_jobs.size(); ++ji) {
        size_t v_idx = filter_jobs[ji].variant_idx;
        const auto& flt = job_filters[ji];
        bool dup = false;
        for (const auto& c : candidates) {
            if (c.variant_idx == v_idx && c.filters == flt) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            candidates.push_back({v_idx, std::move(job_filters[ji]), filter_jobs[ji].strategy});
        }
    }

    if (opts.verbose) {
        std::cout << "Content: " << (content.is_photographic ? "photo" : "graphic")
                  << " gradient=" << content.gradient_strength
                  << " entropy_r=" << content.entropy_r
                  << " unique=" << content.unique_colors << "\n";
        std::cout << "Unique image variants: " << variants.size()
                  << ", unique filtered candidates: " << candidates.size() << "\n";
    }

    // Step 4: Proxy-compress all unique candidates in parallel
    struct TrialResult {
        size_t candidate_idx;
        std::vector<uint8_t> zlib_data;
        size_t proxy_total_size;
    };

    std::vector<TrialResult> results(candidates.size());
    parallel_for(candidates.size(), threads, [&](size_t ci) {
        const auto& cand = candidates[ci];
        const auto& var = variants[cand.variant_idx];
        const Image& work = var.work;
        size_t raw_ss = work.raw_scanline_size();

        TrialResult tr;
        tr.candidate_idx = ci;
        // The filtered stream is only needed for this proxy pass; keeping it
        // in every TrialResult held one full copy of the image per candidate.
        std::vector<uint8_t> filtered_data = build_filtered_stream(work, cand.filters);

        DeflateOptions dopts;
        dopts.level = (opts.level <= 3) ? cand.strategy.deflate_level : CompressionLevel::Best;
        dopts.iterations = 1;
        dopts.adaptive_blocks = !is_huge;
        dopts.chain_depth = 0;
        dopts.max_block_size = 65536;
        dopts.row_stride = static_cast<int>(raw_ss + 1);

        tr.zlib_data = zlib_compress(filtered_data, dopts);
        tr.proxy_total_size = tr.zlib_data.size() + var.chunk_overhead;
        results[ci] = std::move(tr);
    });
    if (opts.verbose) {
        for (size_t ci = 0; ci < results.size(); ++ci)
            std::cout << "  Proxy " << candidates[ci].strategy.name
                      << ": " << results[ci].proxy_total_size << " bytes\n";
    }

    std::sort(results.begin(), results.end(),
        [](const TrialResult& a, const TrialResult& b) {
            return a.proxy_total_size < b.proxy_total_size;
        });

    if (opts.verbose && !results.empty()) {
        std::cout << "Best proxy: " << candidates[results[0].candidate_idx].strategy.name
                  << " (" << results[0].proxy_total_size << " bytes)\n";
    }

    // Step 5: Re-compress the top distinct proxy winners with full Ultra / multi-iteration DEFLATE
    size_t recompress_count = is_huge ? 2 : (is_large ? 3 : 4);
    recompress_count = std::min(recompress_count, results.size());

    // Ensure if there are multiple preprocessed variants, the best candidate from the
    // second-best variant is also included in re-compression so a slightly worse proxy
    // on a different color representation gets a fair Ultra optimal parse trial.
    if (variants.size() > 1 && results.size() > recompress_count) {
        size_t top_v = candidates[results[0].candidate_idx].variant_idx;
        bool has_other_v = false;
        for (size_t ri = 1; ri < recompress_count; ++ri) {
            if (candidates[results[ri].candidate_idx].variant_idx != top_v) {
                has_other_v = true;
                break;
            }
        }
        if (!has_other_v) {
            for (size_t ri = recompress_count; ri < results.size(); ++ri) {
                if (candidates[results[ri].candidate_idx].variant_idx != top_v) {
                    std::swap(results[recompress_count - 1], results[ri]);
                    break;
                }
            }
        }
    }

    struct FinalCandidate {
        std::vector<uint8_t> png_data;
        std::string name;
    };
    std::vector<FinalCandidate> finals(recompress_count);
    parallel_for(recompress_count, threads, [&](size_t ri) {
        const auto& tr = results[ri];
        const auto& cand = candidates[tr.candidate_idx];
        const Image& work = variants[cand.variant_idx].work;
        const Strategy& strat = cand.strategy;

        DeflateOptions dopts;
        dopts.level = strat.deflate_level;
        dopts.iterations = strat.deflate_iterations;
        dopts.adaptive_blocks = true;
        dopts.row_stride = static_cast<int>(work.raw_scanline_size() + 1);
        if (is_huge) {
            // Cap (never raise) the optimal-parse effort on big images. The
            // old code forced 5 iterations at Best even for -o 1..3, which made
            // ordinary screenshots take minutes at the "fast" levels.
            int cap = (raw_pixels <= 2621440) ? 5 : (raw_pixels <= 10485760) ? 3 : 2;
            dopts.iterations = std::min(dopts.iterations, cap);
            if (opts.level >= 7 && dopts.level < CompressionLevel::Best)
                dopts.level = CompressionLevel::Best;
        } else if (is_large) {
            dopts.iterations = std::min(dopts.iterations, 4);
        }
        dopts.chain_depth = 0;

        // If full settings exceed proxy settings, re-compress the already-filtered stream
        WriteOptions wopts;
        wopts.filters = cand.filters;
        wopts.deflate = dopts;

        PNGWriter writer;
        auto png_out = writer.write(work, wopts);

        // Also check if the proxy's 1-iteration stream was somehow smaller
        if (dopts.level != CompressionLevel::Best || dopts.iterations > 1) {
            WriteOptions pwopts;
            pwopts.filters = cand.filters;
            pwopts.deflate.level = CompressionLevel::Best;
            pwopts.deflate.iterations = 1;
            pwopts.deflate.adaptive_blocks = !is_huge;
            pwopts.deflate.max_block_size = 65536;
            pwopts.deflate.row_stride = static_cast<int>(work.raw_scanline_size() + 1);
            // Only build proxy PNG if proxy zlib + chunk overhead could beat png_out
            if (tr.proxy_total_size + 57 < png_out.size()) {
                auto proxy_png = writer.write(work, pwopts);
                if (proxy_png.size() < png_out.size()) {
                    png_out = std::move(proxy_png);
                }
            }
        }

        finals[ri] = FinalCandidate{std::move(png_out), strat.name + "-actual"};
    });
    if (opts.verbose) {
        for (size_t ri = 0; ri < finals.size(); ++ri)
            std::cout << "  Actual " << finals[ri].name << ": "
                      << finals[ri].png_data.size() << " bytes PNG\n";
    }

    std::vector<size_t> order(finals.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return finals[a].png_data.size() < finals[b].png_data.size();
    });

    // Never trust an encoder output without decoding it: pick the smallest
    // candidate that round-trips to exactly the input pixels.
    for (size_t idx : order) {
        std::string why;
        if (verify_png(finals[idx].png_data, img_in, &why)) {
            CompressResult best_result;
            best_result.data = std::move(finals[idx].png_data);
            best_result.strategy_name = finals[idx].name;
            best_result.time_seconds = total.elapsed_seconds();
            best_result.original_size = img.pixels.size();
            return best_result;
        }
        std::cerr << "warning: candidate " << finals[idx].name
                  << " failed verification (" << why << "), discarded\n";
    }
    throw std::runtime_error("no compressed candidate passed verification");
}

bool verify_png(const std::vector<uint8_t>& png, const Image& reference, std::string* why) {
    if (png.empty()) {
        if (why) *why = "empty output";
        return false;
    }
    PNGReader reader;
    auto decoded = reader.read(png);
    if (!decoded) {
        if (why) *why = "re-decode failed: " + reader.error();
        return false;
    }
    if (!reader.warnings().empty()) {
        if (why) *why = "re-decode warning: " + reader.warnings().front();
        return false;
    }
    return images_equivalent(reference, *decoded, why);
}

CompressResult compress_safe(const Image& img, const CompressOptions& opts) {
    // Most conservative encoding: original colour format, simple filters,
    // standard DEFLATE. Used when every optimized candidate failed.
    Image work = img;
    apply_strip_ancillary(work, opts.strip_ancillary, opts.keep_color_info);
    FilterOptions fopts;
    fopts.level = 0;
    WriteOptions wopts;
    wopts.filters = optimize_filters(work, fopts);
    wopts.deflate.level = CompressionLevel::Default;
    wopts.deflate.iterations = 1;
    wopts.deflate.row_stride = static_cast<int>(work.raw_scanline_size() + 1);
    PNGWriter writer;
    CompressResult r;
    r.data = writer.write(work, wopts);
    r.strategy_name = "safe-fallback";
    r.original_size = img.pixels.size();
    return r;
}

CompressResult compress(const Image& img, const CompressOptions& opts) {
    std::string first_error;
    try {
        if (!opts.multi_strategy || opts.level <= 0) {
            auto r = compress_single(img, opts);
            std::string why;
            if (verify_png(r.data, img, &why)) return r;
            first_error = "single-strategy output failed verification (" + why + ")";
        } else {
            return compress_multi(img, opts);
        }
    } catch (const std::bad_alloc&) {
        first_error = "out of memory";
    } catch (const std::exception& e) {
        first_error = e.what();
    }
    if (opts.verbose || !first_error.empty())
        std::cerr << "warning: " << first_error << "; using safe fallback encoding\n";
    auto r = compress_safe(img, opts);
    std::string why;
    if (!verify_png(r.data, img, &why))
        throw std::runtime_error("safe fallback encoding failed verification: " + why);
    return r;
}

} // namespace fpng

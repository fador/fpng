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

    bool multi_frame = (img.is_animated && img.frames.size() > 1);
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

    bool multi_frame = (work.is_animated && work.frames.size() > 1);
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

CompressResult compress(const Image& img_in, const CompressOptions& opts) {
    if (!opts.multi_strategy || opts.level <= 0) {
        return compress_single(img_in, opts);
    }

    Timer total;

    Image img = img_in;
    apply_strip_ancillary(img, opts.strip_ancillary, opts.keep_color_info);

    // Analyze image content to guide strategy selection
    ImageContent content = analyze_content(img);
    size_t raw_pixels = img.pixels.size();

    auto strategies = get_strategies(opts.level);
    bool is_large  = (raw_pixels >= 262144);
    bool is_huge   = (raw_pixels >= 1048576);
    bool multi_frame = (img.is_animated && img.frames.size() > 1);

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

    for (const auto& s : filtered) {
        size_t v_idx = get_or_create_variant(s.alpha_zero, s.color_reduce, s.palette_sort);
        bool exists = false;
        for (const auto& fj : filter_jobs) {
            if (!fj.is_uniform && fj.variant_idx == v_idx &&
                fj.strategy.filter_level == s.filter_level) {
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
    {
        int f_threads = std::min(threads, static_cast<int>(filter_jobs.size()));
        for (size_t batch_start = 0; batch_start < filter_jobs.size(); batch_start += f_threads) {
            size_t batch_end = std::min(batch_start + static_cast<size_t>(f_threads), filter_jobs.size());
            std::vector<std::future<std::vector<FilterType>>> futures;
            for (size_t ji = batch_start; ji < batch_end; ++ji) {
                futures.push_back(std::async(std::launch::async, [&, ji]() {
                    const auto& job = filter_jobs[ji];
                    const Image& work = variants[job.variant_idx].work;
                    if (job.is_uniform) {
                        return std::vector<FilterType>(work.height, job.uniform_type);
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
                    return optimize_filters(work, fopts);
                }));
            }
            for (size_t ji = batch_start; ji < batch_end; ++ji) {
                job_filters[ji] = futures[ji - batch_start].get();
            }
        }
    }

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
        std::vector<uint8_t> filtered_data;
        std::vector<uint8_t> zlib_data;
        size_t proxy_total_size;
    };

    std::vector<TrialResult> results(candidates.size());
    {
        int p_threads = std::min(threads, static_cast<int>(candidates.size()));
        for (size_t batch_start = 0; batch_start < candidates.size(); batch_start += p_threads) {
            size_t batch_end = std::min(batch_start + static_cast<size_t>(p_threads), candidates.size());
            std::vector<std::future<TrialResult>> futures;
            for (size_t ci = batch_start; ci < batch_end; ++ci) {
                futures.push_back(std::async(std::launch::async, [&, ci]() {
                    const auto& cand = candidates[ci];
                    const auto& var = variants[cand.variant_idx];
                    const Image& work = var.work;
                    size_t raw_ss = work.raw_scanline_size();

                    TrialResult tr;
                    tr.candidate_idx = ci;
                    tr.filtered_data = build_filtered_stream(work, cand.filters);

                    DeflateOptions dopts;
                    dopts.level = (opts.level <= 3) ? cand.strategy.deflate_level : CompressionLevel::Best;
                    dopts.iterations = 1;
                    dopts.adaptive_blocks = !is_huge;
                    dopts.chain_depth = 0;
                    dopts.max_block_size = 65536;
                    dopts.row_stride = static_cast<int>(raw_ss + 1);

                    tr.zlib_data = zlib_compress(tr.filtered_data, dopts);
                    tr.proxy_total_size = tr.zlib_data.size() + var.chunk_overhead;
                    return tr;
                }));
            }
            for (size_t ci = batch_start; ci < batch_end; ++ci) {
                results[ci] = futures[ci - batch_start].get();
                if (opts.verbose) {
                    std::cout << "  Proxy " << candidates[ci].strategy.name
                              << ": " << results[ci].proxy_total_size << " bytes\n";
                }
            }
        }
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

    {
        int r_threads = std::min(threads, static_cast<int>(recompress_count));
        for (size_t batch_start = 0; batch_start < recompress_count; batch_start += r_threads) {
            size_t batch_end = std::min(batch_start + static_cast<size_t>(r_threads), recompress_count);
            std::vector<std::future<FinalCandidate>> futures;
            for (size_t ri = batch_start; ri < batch_end; ++ri) {
                futures.push_back(std::async(std::launch::async, [&, ri]() {
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
                        dopts.iterations = (raw_pixels <= 2621440) ? 5
                                         : (raw_pixels <= 10485760) ? 3 : 2;
                        if (dopts.level < CompressionLevel::Best)
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

                    return FinalCandidate{std::move(png_out), strat.name + "-actual"};
                }));
            }
            for (size_t ri = batch_start; ri < batch_end; ++ri) {
                finals[ri] = futures[ri - batch_start].get();
                if (opts.verbose) {
                    std::cout << "  Actual " << finals[ri].name << ": "
                              << finals[ri].png_data.size() << " bytes PNG\n";
                }
            }
        }
    }

    size_t best_idx = 0;
    for (size_t ri = 1; ri < finals.size(); ++ri) {
        if (finals[ri].png_data.size() < finals[best_idx].png_data.size()) {
            best_idx = ri;
        }
    }

    CompressResult best_result;
    best_result.data = std::move(finals[best_idx].png_data);
    best_result.strategy_name = finals[best_idx].name;
    best_result.time_seconds = total.elapsed_seconds();
    best_result.original_size = img.pixels.size();
    return best_result;
}

} // namespace fpng

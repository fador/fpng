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
    for (size_t i = 0; i < size; ++i) sum += data[i];
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

// Compress trial data and return size
size_t trial_compress_size(const std::vector<uint8_t>& data) {
    DeflateOptions dopts;
    dopts.level = CompressionLevel::Store;
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
        // Hill-climbing with restarts (levels 5-6) or Genetic Algorithm (level 7+ or forced)
        bool use_ga = (opts.level >= 7) || opts.use_genetic;

        if (use_ga) {
            // === Genetic Algorithm ===
            // Auto-scale population based on image size
            int POP = opts.ga_population;
            int GENS = opts.ga_generations;
            if (height < 64) {
                POP = std::min(POP, 10);
                GENS = std::min(GENS, 15);
            } else if (height < 256) {
                POP = std::min(POP, 20);
                GENS = std::min(GENS, 30);
            } else {
                POP = std::min(POP, 30);
                GENS = std::min(GENS, 50);
            }
            const int ELITE = std::max(1, POP / 10);

            struct Individual {
                std::vector<FilterType> filters;
                size_t compressed_size = std::numeric_limits<size_t>::max();
            };

            // Fitness: use byte entropy (fast, correlates well with compressibility)
            auto fitness = [&](const std::vector<FilterType>& filters) -> size_t {
                std::vector<uint8_t> filtered;
                std::vector<uint8_t> prev(raw_ss, 0);
                for (size_t y = 0; y < height; ++y) {
                    const uint8_t* src = img.pixels.data() + y * raw_ss;
                    std::vector<uint8_t> row(raw_ss + 1);
                    FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
                    filter_scanline(ft, src, row.data(), bpp, raw_ss,
                                     y > 0 ? prev.data() : nullptr);
                    filtered.insert(filtered.end(), row.begin(), row.end());
                    std::memcpy(prev.data(), src, raw_ss);
                }
                // Use byte entropy * 100 as integer proxy (lower = better)
                double ent = byte_entropy(filtered.data(), filtered.size());
                // Also add a small penalty for filter variety (encourages runs)
                int switches = 0;
                for (size_t y = 1; y < filters.size(); ++y)
                    if (filters[y] != filters[y-1]) ++switches;
                return static_cast<size_t>(ent * 100.0) + switches * 2;
            };

            // Initialize population
            std::vector<Individual> pop(POP);

            // Seed 1: entropy-based heuristic
            std::vector<uint8_t> prev_h(raw_ss, 0);
            for (size_t y = 0; y < height; ++y) {
                const uint8_t* src = img.pixels.data() + y * raw_ss;
                FilterType best = FilterType::None;
                double best_cost = std::numeric_limits<double>::max();
                for (int ft = 0; ft <= 4; ++ft) {
                    auto type = static_cast<FilterType>(ft);
                    double cost = filter_cost(type, src, raw_ss, bpp,
                                               y > 0 ? prev_h.data() : nullptr);
                    if (cost < best_cost) { best_cost = cost; best = type; }
                }
                pop[0].filters.push_back(best);
                std::memcpy(prev_h.data(), src, raw_ss);
            }
            pop[0].compressed_size = fitness(pop[0].filters);

            // Seeds 2-3: all None, all Paeth
            pop[1].filters.assign(height, FilterType::None);
            pop[1].compressed_size = fitness(pop[1].filters);
            pop[2].filters.assign(height, FilterType::Paeth);
            pop[2].compressed_size = fitness(pop[2].filters);

            // Seeds 4-N: random perturbations of the heuristic solution
            std::mt19937 rng(42);
            for (int p = 3; p < POP; ++p) {
                pop[p].filters = pop[0].filters;
                int flips = static_cast<int>(height) / 10 + 2;
                for (int f = 0; f < flips; ++f) {
                    size_t row = rng() % height;
                    pop[p].filters[row] = static_cast<FilterType>(rng() % 5);
                }
                pop[p].compressed_size = fitness(pop[p].filters);
            }

            // Sort by fitness
            std::sort(pop.begin(), pop.end(),
                [](const Individual& a, const Individual& b) {
                    return a.compressed_size < b.compressed_size;
                });

            size_t best_overall = pop[0].compressed_size;

            // Evolution loop
            for (int gen = 0; gen < GENS; ++gen) {
                std::vector<Individual> next;
                next.reserve(POP);

                // Elitism
                for (int e = 0; e < ELITE; ++e)
                    next.push_back(pop[e]);

                // Crossover + mutation
                while (static_cast<int>(next.size()) < POP) {
                    // Tournament selection (pick 3, best wins)
                    int t1 = rng() % POP, t2 = rng() % POP, t3 = rng() % POP;
                    int p1 = std::min({t1, t2, t3}, [&](int a, int b) {
                        return pop[a].compressed_size < pop[b].compressed_size; });
                    t1 = rng() % POP; t2 = rng() % POP; t3 = rng() % POP;
                    int p2 = std::min({t1, t2, t3}, [&](int a, int b) {
                        return pop[a].compressed_size < pop[b].compressed_size; });

                    // Uniform crossover
                    Individual child;
                    child.filters.resize(height);
                    for (size_t r = 0; r < height; ++r) {
                        child.filters[r] = (rng() & 1) ?
                            pop[p1].filters[r] : pop[p2].filters[r];
                    }

                    // Mutation: flip ~5% of rows, with local bursts
                    double mut_rate = 0.05;
                    if (gen > GENS / 2) mut_rate = 0.02; // reduce later

                    for (size_t r = 0; r < height; ++r) {
                        if ((rng() % 1000) < static_cast<int>(mut_rate * 1000)) {
                            // Local burst: flip this and nearby rows
                            int burst = (rng() % 3) + 1;
                            for (int b = 0; b < burst; ++b) {
                                size_t rr = r + b;
                                if (rr < height)
                                    child.filters[rr] = static_cast<FilterType>(rng() % 5);
                            }
                        }
                    }

                    child.compressed_size = fitness(child.filters);
                    next.push_back(std::move(child));
                }

                pop = std::move(next);
                std::sort(pop.begin(), pop.end(),
                    [](const Individual& a, const Individual& b) {
                        return a.compressed_size < b.compressed_size;
                    });

                if (pop[0].compressed_size < best_overall) {
                    best_overall = pop[0].compressed_size;
                }

                // Early termination if no improvement for 20 generations
                if (gen > 20 && gen % 10 == 0) {
                    bool improved = false;
                    for (int i = 0; i < ELITE; ++i) {
                        if (pop[i].compressed_size < best_overall * 0.99) {
                            improved = true; break;
                        }
                    }
                    if (!improved && pop[0].compressed_size <= best_overall) break;
                }
            }

            result = pop[0].filters;

            // Re-evaluate top 3 GA winners with byte entropy (faster than deflate)
            // Entropy correlates with compressibility and is much faster
            {
                auto fitness_real = [&](const std::vector<FilterType>& filters) -> size_t {
                    std::vector<uint8_t> filtered;
                    std::vector<uint8_t> prev(raw_ss, 0);
                    for (size_t y = 0; y < height; ++y) {
                        const uint8_t* src = img.pixels.data() + y * raw_ss;
                        std::vector<uint8_t> row(raw_ss + 1);
                        FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
                        filter_scanline(ft, src, row.data(), bpp, raw_ss,
                                         y > 0 ? prev.data() : nullptr);
                        filtered.insert(filtered.end(), row.begin(), row.end());
                        std::memcpy(prev.data(), src, raw_ss);
                    }
                    double ent = byte_entropy(filtered.data(), filtered.size());
                    return static_cast<size_t>(ent * 100.0);
                };

                size_t best_real = fitness_real(result);
                for (int i = 1; i < std::min(POP, 5); ++i) {
                    size_t sz = fitness_real(pop[i].filters);
                    if (sz < best_real) {
                        best_real = sz;
                        result = pop[i].filters;
                    }
                }
            }

        } else {
            // === Stochastic hill-climbing with restarts (levels 5-6) ===
            const int MAX_STEPS = static_cast<int>(height) * 3;

            std::mt19937 rng(42);
            std::vector<FilterType> best_filters;
            size_t best_size = std::numeric_limits<size_t>::max();

            auto fitness_hc = [&](const std::vector<FilterType>& filters) -> size_t {
                std::vector<uint8_t> filtered;
                std::vector<uint8_t> prev(raw_ss, 0);
                for (size_t y = 0; y < height; ++y) {
                    const uint8_t* src = img.pixels.data() + y * raw_ss;
                    std::vector<uint8_t> row(raw_ss + 1);
                    FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
                    filter_scanline(ft, src, row.data(), bpp, raw_ss,
                                     y > 0 ? prev.data() : nullptr);
                    filtered.insert(filtered.end(), row.begin(), row.end());
                    std::memcpy(prev.data(), src, raw_ss);
                }
                double ent = byte_entropy(filtered.data(), filtered.size());
                return static_cast<size_t>(ent * 100.0);
            };

            // Initial solutions to try
            std::vector<std::vector<FilterType>> initials;

            // Entropy heuristic
            {
                std::vector<FilterType> f;
                std::vector<uint8_t> prev_h(raw_ss, 0);
                for (size_t y = 0; y < height; ++y) {
                    const uint8_t* src = img.pixels.data() + y * raw_ss;
                    FilterType best = FilterType::None;
                    double best_cost = std::numeric_limits<double>::max();
                    for (int ft = 0; ft <= 4; ++ft) {
                        auto type = static_cast<FilterType>(ft);
                        double cost = filter_cost(type, src, raw_ss, bpp,
                                                   y > 0 ? prev_h.data() : nullptr);
                        if (cost < best_cost) { best_cost = cost; best = type; }
                    }
                    f.push_back(best);
                    std::memcpy(prev_h.data(), src, raw_ss);
                }
                initials.push_back(f);
            }

            // All None
            initials.push_back(std::vector<FilterType>(height, FilterType::None));
            // All Paeth
            initials.push_back(std::vector<FilterType>(height, FilterType::Paeth));
            // All Up
            initials.push_back(std::vector<FilterType>(height, FilterType::Up));
            // All Average
            initials.push_back(std::vector<FilterType>(height, FilterType::Average));
            // All Sub
            initials.push_back(std::vector<FilterType>(height, FilterType::Sub));

            // MinSum heuristic
            {
                std::vector<FilterType> f;
                std::vector<uint8_t> prev_i(raw_ss, 0);
                for (size_t y = 0; y < height; ++y) {
                    const uint8_t* src = img.pixels.data() + y * raw_ss;
                    std::vector<uint8_t> row(raw_ss + 1);
                    FilterType best = FilterType::None;
                    uint64_t best_cost = std::numeric_limits<uint64_t>::max();
                    for (int ft = 0; ft <= 4; ++ft) {
                        auto type = static_cast<FilterType>(ft);
                        filter_scanline(type, src, row.data(), bpp, raw_ss,
                                         y > 0 ? prev_i.data() : nullptr);
                        uint64_t cost = sum_abs(row.data() + 1, raw_ss);
                        if (cost < best_cost) { best_cost = cost; best = type; }
                    }
                    f.push_back(best);
                    std::memcpy(prev_i.data(), src, raw_ss);
                }
                initials.push_back(f);
            }

            for (auto& current : initials) {
                size_t current_size = fitness_hc(current);

                // Hill climb
                int steps_without_improvement = 0;
                for (int step = 0; step < MAX_STEPS && steps_without_improvement < 50; ++step) {
                    // Try flipping a random contiguous block of rows
                    size_t start = rng() % height;
                    size_t count = (rng() % std::min(size_t(5), height - start)) + 1;
                    std::vector<FilterType> neighbor = current;
                    for (size_t r = start; r < start + count && r < height; ++r)
                        neighbor[r] = static_cast<FilterType>(rng() % 5);

                    size_t neighbor_size = fitness_hc(neighbor);
                    if (neighbor_size < current_size) {
                        current = std::move(neighbor);
                        current_size = neighbor_size;
                        steps_without_improvement = 0;
                    } else {
                        ++steps_without_improvement;
                    }
                }

                if (current_size < best_size) {
                    best_size = current_size;
                    best_filters = std::move(current);
                }
            }

            result = best_filters;
        }
    }

    return result;
}

} // namespace fpng

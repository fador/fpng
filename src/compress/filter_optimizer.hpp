#pragma once
#include "image/image.hpp"
#include "png/filter.hpp"
#include <vector>
namespace fpng {
struct FilterOptions {
    int level = 1;           // 0=MinSum, 1-2=entropy, 3-4=brute-force, 5-6=hill-climb, 7+=GA
    int window_size = 2;     // Lookahead window for brute-force, K for DP
    bool use_genetic = false; // force GA even at lower levels
    int ga_population = 30;  // population size for GA
    int ga_generations = 60; // max generations for GA
};
std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts = {});
} // namespace fpng

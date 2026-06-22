#pragma once
#include "image/image.hpp"
#include "png/filter.hpp"
#include <vector>
namespace fpng {
struct FilterOptions {
    int level = 1;           // 0=MinSum, 1-2=entropy, 3-4=brute-force windowed, 5+=DP K-state
    int window_size = 2;     // Lookahead window for brute-force, K for DP
    bool use_genetic = false;
};
std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts = {});
} // namespace fpng

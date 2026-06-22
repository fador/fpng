#pragma once
#include "image/image.hpp"
#include "png/filter.hpp"
#include <vector>
namespace fpng {
struct FilterOptions {
    int level = 0;
    int window_size = 2;
    bool use_genetic = false;
};
std::vector<FilterType> optimize_filters(const Image& img, const FilterOptions& opts = {});
} // namespace fpng

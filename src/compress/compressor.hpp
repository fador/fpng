#pragma once
#include "image/image.hpp"
#include <vector>
#include <cstdint>
namespace fpng {
struct CompressOptions {
    int level = 9;
    bool strip_ancillary = false;
    bool keep_color_info = true;
};
struct CompressResult {
    std::vector<uint8_t> data;
    double time_seconds = 0;
    size_t original_size = 0;
};
CompressResult compress(const Image& img, const CompressOptions& opts = {});
} // namespace fpng

#include "preprocess/palette_sorter.hpp"

#include <algorithm>
#include <vector>
#include <cmath>

namespace fpng {

void sort_palette(Image& img) {
    if (img.color_type != 3 || img.palette.empty()) return;

    size_t num_colors = img.palette.size() / 3;
    if (num_colors <= 1) return;

    // Compute luminance for sorting
    std::vector<std::pair<double, size_t>> lum;
    for (size_t i = 0; i < num_colors; ++i) {
        double r = img.palette[i * 3];
        double g = img.palette[i * 3 + 1];
        double b = img.palette[i * 3 + 2];
        double l = 0.299 * r + 0.587 * g + 0.114 * b;
        lum.push_back({l, i});
    }

    std::sort(lum.begin(), lum.end());

    // Build permutation
    std::vector<uint8_t> perm(num_colors);
    for (size_t i = 0; i < num_colors; ++i)
        perm[lum[i].second] = static_cast<uint8_t>(i);

    // Reorder palette
    std::vector<uint8_t> new_palette(num_colors * 3);
    std::vector<uint8_t> new_alpha(num_colors, 255);
    for (size_t i = 0; i < num_colors; ++i) {
        size_t src = lum[i].second;
        new_palette[i * 3 + 0] = img.palette[src * 3 + 0];
        new_palette[i * 3 + 1] = img.palette[src * 3 + 1];
        new_palette[i * 3 + 2] = img.palette[src * 3 + 2];
        if (src < img.alpha_palette.size())
            new_alpha[i] = img.alpha_palette[src];
    }
    img.palette = std::move(new_palette);
    if (!img.alpha_palette.empty())
        img.alpha_palette = std::move(new_alpha);

    // Remap pixels (packed bit depths store multiple indices per byte)
    if (img.bit_depth >= 8) {
        for (auto& p : img.pixels) {
            if (p < perm.size()) p = perm[p];
        }
    } else {
        int bd = img.bit_depth;
        int per = 8 / bd;
        uint8_t mask = static_cast<uint8_t>((1u << bd) - 1);
        size_t row_bytes = img.raw_scanline_size();
        for (size_t y = 0; y < img.height; ++y) {
            uint8_t* row = img.pixels.data() + y * row_bytes;
            for (size_t bx = 0; bx < row_bytes; ++bx) {
                uint8_t byte = row[bx];
                uint8_t outb = 0;
                for (int k = 0; k < per; ++k) {
                    size_t pixel = bx * static_cast<size_t>(per) + k;
                    int shift = 8 - bd * (k + 1);
                    uint8_t idx = static_cast<uint8_t>((byte >> shift) & mask);
                    uint8_t mapped = (pixel < img.width && idx < perm.size())
                                         ? perm[idx] : idx;
                    outb = static_cast<uint8_t>(outb | (mapped << shift));
                }
                row[bx] = outb;
            }
        }
    }
}

} // namespace fpng

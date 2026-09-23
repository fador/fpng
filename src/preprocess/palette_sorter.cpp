#include "preprocess/palette_sorter.hpp"

#include <algorithm>
#include <vector>
#include <cmath>

namespace fpng {

void sort_palette(Image& img) {
    if (img.color_type != 3 || img.palette.empty()) return;

    size_t num_colors = img.palette.size() / 3;
    if (num_colors <= 1) return;

    // For sub-byte bit depths, sort by pixel frequency so the most common
    // index becomes 0. Packed bytes then contain more zero bits, which
    // compresses better with LZ77+Huffman (filters mix bits across pixels).
    // For 8-bit palettes, Huffman cost is permutation-invariant so luminance
    // sorting only helps via filter residuals; keep it as the default.
    bool sub_byte = img.bit_depth < 8;

    if (sub_byte) {
        // Count pixel frequencies per palette index.
        std::vector<uint32_t> freq(num_colors, 0);
        int bd = img.bit_depth;
        int per = 8 / bd;
        uint8_t mask = static_cast<uint8_t>((1u << bd) - 1);
        size_t row_bytes = img.raw_scanline_size();
        for (size_t y = 0; y < img.height; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_bytes;
            for (size_t bx = 0; bx < row_bytes; ++bx) {
                uint8_t byte = row[bx];
                for (int k = 0; k < per; ++k) {
                    size_t pixel = bx * static_cast<size_t>(per) + k;
                    if (pixel >= img.width) break;
                    int shift = 8 - bd * (k + 1);
                    uint8_t idx = static_cast<uint8_t>((byte >> shift) & mask);
                    if (idx < num_colors) freq[idx]++;
                }
            }
        }

        // Sort by descending frequency; break ties by luminance.
        std::vector<size_t> order(num_colors);
        for (size_t i = 0; i < num_colors; ++i) order[i] = i;
        std::vector<double> lum(num_colors);
        for (size_t i = 0; i < num_colors; ++i) {
            double r = img.palette[i * 3], g = img.palette[i * 3 + 1], b = img.palette[i * 3 + 2];
            lum[i] = 0.299 * r + 0.587 * g + 0.114 * b;
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (freq[a] != freq[b]) return freq[a] > freq[b];
            return lum[a] < lum[b];
        });

        std::vector<uint8_t> perm(num_colors);
        for (size_t i = 0; i < num_colors; ++i)
            perm[order[i]] = static_cast<uint8_t>(i);

        // Reorder palette + alpha.
        std::vector<uint8_t> new_palette(num_colors * 3);
        std::vector<uint8_t> new_alpha(num_colors, 255);
        for (size_t i = 0; i < num_colors; ++i) {
            size_t src = order[i];
            new_palette[i * 3 + 0] = img.palette[src * 3 + 0];
            new_palette[i * 3 + 1] = img.palette[src * 3 + 1];
            new_palette[i * 3 + 2] = img.palette[src * 3 + 2];
            if (src < img.alpha_palette.size())
                new_alpha[i] = img.alpha_palette[src];
        }
        img.palette = std::move(new_palette);
        if (!img.alpha_palette.empty())
            img.alpha_palette = std::move(new_alpha);

        // Remap pixels.
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
        return;
    }

    // 8-bit palette: sort by luminance, with opaque entries last for tRNS trim.
    std::vector<std::pair<double, size_t>> lum;
    for (size_t i = 0; i < num_colors; ++i) {
        double r = img.palette[i * 3];
        double g = img.palette[i * 3 + 1];
        double b = img.palette[i * 3 + 2];
        double l = 0.299 * r + 0.587 * g + 0.114 * b;
        lum.push_back({l, i});
    }

    std::sort(lum.begin(), lum.end(), [&](const std::pair<double, size_t>& a,
                                          const std::pair<double, size_t>& b) {
        // Opaque entries last so trailing 255-trim can shorten tRNS.
        uint8_t aa = (a.second < img.alpha_palette.size()) ? img.alpha_palette[a.second] : 255;
        uint8_t bb = (b.second < img.alpha_palette.size()) ? img.alpha_palette[b.second] : 255;
        int ka = (aa == 255) ? 1 : 0;
        int kb = (bb == 255) ? 1 : 0;
        if (ka != kb) return ka < kb;
        return a.first < b.first;
    });

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

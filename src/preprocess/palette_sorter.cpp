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

    // 8-bit palette: optimize palette ordering using TSP heuristic based on
    // pixel adjacency and Euclidean RGB distance. Non-opaque entries come first
    // so trailing 255-trim can shorten tRNS.
    std::vector<size_t> non_opaque;
    std::vector<size_t> opaque;
    for (size_t i = 0; i < num_colors; ++i) {
        uint8_t a = (i < img.alpha_palette.size()) ? img.alpha_palette[i] : 255;
        if (a < 255) non_opaque.push_back(i);
        else opaque.push_back(i);
    }

    // Compute pixel adjacency matrix for 8-bit scanlines
    size_t row_bytes = img.raw_scanline_size();
    std::vector<uint16_t> adj(num_colors * num_colors, 0);
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_bytes;
        for (size_t x = 0; x + 1 < img.width; ++x) {
            uint8_t u = row[x];
            uint8_t v = row[x + 1];
            if (u != v && u < num_colors && v < num_colors) {
                if (adj[u * num_colors + v] < 65535) adj[u * num_colors + v]++;
                if (adj[v * num_colors + u] < 65535) adj[v * num_colors + u]++;
            }
        }
    }
    for (size_t y = 0; y + 1 < img.height; ++y) {
        const uint8_t* row1 = img.pixels.data() + y * row_bytes;
        const uint8_t* row2 = img.pixels.data() + (y + 1) * row_bytes;
        for (size_t x = 0; x < img.width; ++x) {
            uint8_t u = row1[x];
            uint8_t v = row2[x];
            if (u != v && u < num_colors && v < num_colors) {
                if (adj[u * num_colors + v] < 65535) adj[u * num_colors + v]++;
                if (adj[v * num_colors + u] < 65535) adj[v * num_colors + u]++;
            }
        }
    }

    auto solve_tour = [&](const std::vector<size_t>& subset) -> std::vector<size_t> {
        if (subset.size() <= 2) return subset;

        auto dist = [&](size_t u, size_t v) -> double {
            int dr = static_cast<int>(img.palette[u * 3]) - static_cast<int>(img.palette[v * 3]);
            int dg = static_cast<int>(img.palette[u * 3 + 1]) - static_cast<int>(img.palette[v * 3 + 1]);
            int db = static_cast<int>(img.palette[u * 3 + 2]) - static_cast<int>(img.palette[v * 3 + 2]);
            double d_rgb = std::sqrt(static_cast<double>(dr * dr + dg * dg + db * db));
            uint16_t co = adj[u * num_colors + v];
            return d_rgb / (1.0 + 8.0 * static_cast<double>(co));
        };

        std::vector<bool> visited(num_colors, false);
        size_t start = subset[0];
        double min_lum = 1e9;
        for (size_t idx : subset) {
            double r = img.palette[idx * 3];
            double g = img.palette[idx * 3 + 1];
            double b = img.palette[idx * 3 + 2];
            double l = 0.299 * r + 0.587 * g + 0.114 * b;
            if (l < min_lum) {
                min_lum = l;
                start = idx;
            }
        }

        std::vector<size_t> path;
        path.reserve(subset.size());
        path.push_back(start);
        visited[start] = true;

        while (path.size() < subset.size()) {
            size_t curr = path.back();
            size_t best_nxt = subset[0];
            double best_d = 1e18;
            for (size_t cand : subset) {
                if (!visited[cand]) {
                    double d = dist(curr, cand);
                    if (d < best_d) {
                        best_d = d;
                        best_nxt = cand;
                    }
                }
            }
            path.push_back(best_nxt);
            visited[best_nxt] = true;
        }

        // 2-opt refinement
        bool improved = true;
        int passes = 0;
        while (improved && passes < 5) {
            improved = false;
            passes++;
            for (size_t i = 1; i + 2 < path.size(); ++i) {
                for (size_t j = i + 1; j + 1 < path.size(); ++j) {
                    double d_cur = dist(path[i - 1], path[i]) + dist(path[j], path[j + 1]);
                    double d_new = dist(path[i - 1], path[j]) + dist(path[i], path[j + 1]);
                    if (d_new < d_cur - 1e-6) {
                        std::reverse(path.begin() + i, path.begin() + j + 1);
                        improved = true;
                    }
                }
            }
        }
        return path;
    };

    std::vector<size_t> tour_non_opaque = solve_tour(non_opaque);
    std::vector<size_t> tour_opaque = solve_tour(opaque);

    std::vector<size_t> order;
    order.reserve(num_colors);
    order.insert(order.end(), tour_non_opaque.begin(), tour_non_opaque.end());
    order.insert(order.end(), tour_opaque.begin(), tour_opaque.end());

    // Build permutation
    std::vector<uint8_t> perm(num_colors);
    for (size_t i = 0; i < num_colors; ++i)
        perm[order[i]] = static_cast<uint8_t>(i);

    // Reorder palette
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
    if (!img.alpha_palette.empty()) {
        // Trim trailing fully opaque entries to minimize tRNS chunk length
        while (!new_alpha.empty() && new_alpha.back() == 255) {
            new_alpha.pop_back();
        }
        img.alpha_palette = std::move(new_alpha);
    }

    // Remap pixels (packed bit depths store multiple indices per byte)
    if (img.bit_depth >= 8) {
        for (auto& p : img.pixels) {
            if (p < perm.size()) p = perm[p];
        }
    } else {
        int bd = img.bit_depth;
        int per = 8 / bd;
        uint8_t mask = static_cast<uint8_t>((1u << bd) - 1);
        row_bytes = img.raw_scanline_size();
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

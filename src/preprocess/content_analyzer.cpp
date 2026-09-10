#include "preprocess/content_analyzer.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace fpng {

ImageContent analyze_content(const Image& img) {
    ImageContent content{};
    size_t bpp = img.bytes_per_pixel();
    size_t row_size = img.raw_scanline_size();
    size_t pixel_count = img.pixel_count();

    if (pixel_count == 0 || bpp == 0 || img.pixels.empty()) return content;

    // Number of pixels actually addressable in a raw scanline. For bit-packed
    // formats (bit_depth < 8) rows are narrower than width*bpp, so iterate by
    // bytes rather than assuming one byte per pixel.
    size_t chans = std::min(bpp, size_t(3));
    size_t row_pixels = row_size / bpp; // floor; safe byte-based bound

    // Gradient analysis
    double total_gradient = 0;
    size_t grad_samples = 0;

    for (size_t y = 1; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * row_size;
        const uint8_t* prev = img.pixels.data() + (y - 1) * row_size;
        for (size_t x = 0; x < row_pixels; ++x) {
            size_t off = x * bpp;
            if (off + chans > row_size) break;
            double g = 0;
            for (size_t c = 0; c < chans; ++c) {
                int diff = static_cast<int>(row[off + c]) -
                           static_cast<int>(prev[off + c]);
                g += diff * diff;
            }
            total_gradient += std::sqrt(g);
            ++grad_samples;
        }
    }

    if (grad_samples > 0)
        content.gradient_strength = total_gradient / grad_samples;

    // Classify
    content.is_photographic = (content.gradient_strength > 5.0);
    content.is_graphics = !content.is_photographic;

    // Count unique colors (sample first 4096 pixels for speed)
    size_t sample_count = std::min(pixel_count, size_t(4096));
    std::vector<uint32_t> color_hashes;
    color_hashes.reserve(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        size_t off = i * bpp;
        if (off + bpp <= img.pixels.size()) {
            uint32_t h = 0;
            for (size_t c = 0; c < std::min(bpp, size_t(4)); ++c)
                h = h * 31 + img.pixels[off + c];
            color_hashes.push_back(h);
        }
    }
    std::sort(color_hashes.begin(), color_hashes.end());
    size_t unique = color_hashes.empty() ? 0 : 1;
    for (size_t i = 1; i < color_hashes.size(); ++i)
        if (color_hashes[i] != color_hashes[i-1]) ++unique;
    content.unique_colors = unique;

    // Entropy per channel (simple Shannon entropy estimate). Walk the actual
    // pixel buffer, not width*height, so bit-packed and 16-bit data stay in
    // bounds and use real channel bytes.
    uint32_t hist[4][256] = {};
    size_t total_pixels = 0;
    for (size_t off = 0; off + bpp <= img.pixels.size() && total_pixels < 65536;
         off += bpp, ++total_pixels) {
        for (size_t c = 0; c < std::min(bpp, size_t(4)); ++c)
            hist[c][img.pixels[off + c]]++;
    }
    if (total_pixels == 0) total_pixels = 1;

    auto entropy = [](const uint32_t* h, size_t total) -> float {
        float e = 0;
        for (int i = 0; i < 256; ++i) {
            if (h[i] > 0) {
                float p = static_cast<float>(h[i]) / total;
                e -= p * std::log2(p);
            }
        }
        return e;
    };

    content.entropy_r = entropy(hist[0], total_pixels);
    content.entropy_g = entropy(hist[1], total_pixels);
    content.entropy_b = entropy(hist[2], total_pixels);
    content.entropy_a = entropy(hist[3], total_pixels);

    return content;
}

} // namespace fpng

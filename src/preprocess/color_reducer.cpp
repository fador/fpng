#include "preprocess/color_reducer.hpp"

#include <unordered_set>
#include <array>

namespace fpng {

void reduce_colors(Image& img) {
    size_t bpp = img.bytes_per_pixel();
    size_t row_size = img.raw_scanline_size();

    // RGB -> Grayscale if R=G=B for all pixels
    if (img.color_type == 2 || img.color_type == 6) {
        bool all_gray = true;
        for (size_t y = 0; y < img.height && all_gray; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                uint8_t r = row[x * bpp + 0];
                uint8_t g = row[x * bpp + 1];
                uint8_t b = row[x * bpp + 2];
                if (r != g || g != b) { all_gray = false; break; }
            }
        }
        if (all_gray) {
            if (img.color_type == 2) {
                // RGB -> Grayscale
                std::vector<uint8_t> new_pixels;
                new_pixels.reserve(img.width * img.height);
                for (size_t y = 0; y < img.height; ++y) {
                    const uint8_t* row = img.pixels.data() + y * row_size;
                    for (size_t x = 0; x < img.width; ++x)
                        new_pixels.push_back(row[x * bpp]);
                }
                img.pixels = std::move(new_pixels);
                img.color_type = 0;
            } else if (img.color_type == 6) {
                // RGBA -> Grayscale+Alpha
                std::vector<uint8_t> new_pixels;
                new_pixels.reserve(img.width * img.height * 2);
                for (size_t y = 0; y < img.height; ++y) {
                    const uint8_t* row = img.pixels.data() + y * row_size;
                    for (size_t x = 0; x < img.width; ++x) {
                        new_pixels.push_back(row[x * bpp]);
                        new_pixels.push_back(row[x * bpp + 3]);
                    }
                }
                img.pixels = std::move(new_pixels);
                img.color_type = 4;
            }
        }
    }

    // Truecolor -> Indexed if <= 256 unique colors
    if (img.color_type == 2 || img.color_type == 6) {
        struct RGBA { uint8_t r, g, b, a;
            bool operator==(const RGBA& o) const { return r==o.r&&g==o.g&&b==o.b&&a==o.a; }
        };
        struct Hash { size_t operator()(RGBA c) const {
            return (uint32_t(c.r)<<24)|(uint32_t(c.g)<<16)|(uint32_t(c.b)<<8)|c.a;
        }};

        std::unordered_set<RGBA, Hash> colors;
        bool too_many = false;
        for (size_t y = 0; y < img.height && !too_many; ++y) {
            const uint8_t* row = img.pixels.data() + y * row_size;
            for (size_t x = 0; x < img.width; ++x) {
                RGBA c;
                c.r = row[x * bpp + 0];
                c.g = row[x * bpp + 1];
                c.b = row[x * bpp + 2];
                c.a = (img.color_type == 6) ? row[x * bpp + 3] : 255;
                colors.insert(c);
                if (colors.size() > 256) { too_many = true; break; }
            }
        }

        if (!too_many && colors.size() <= 256) {
            // Convert to indexed
            std::vector<RGBA> palette(colors.begin(), colors.end());
            img.palette.resize(palette.size() * 3);
            img.alpha_palette.clear();
            bool need_alpha = false;
            for (size_t i = 0; i < palette.size(); ++i) {
                img.palette[i * 3 + 0] = palette[i].r;
                img.palette[i * 3 + 1] = palette[i].g;
                img.palette[i * 3 + 2] = palette[i].b;
                if (palette[i].a < 255) need_alpha = true;
                img.alpha_palette.push_back(palette[i].a);
            }

            // Remap pixels
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.width * img.height);
            for (size_t y = 0; y < img.height; ++y) {
                const uint8_t* row = img.pixels.data() + y * row_size;
                for (size_t x = 0; x < img.width; ++x) {
                    RGBA c;
                    c.r = row[x * bpp + 0];
                    c.g = row[x * bpp + 1];
                    c.b = row[x * bpp + 2];
                    c.a = (img.color_type == 6) ? row[x * bpp + 3] : 255;
                    for (size_t pi = 0; pi < palette.size(); ++pi) {
                        if (palette[pi] == c) { new_pixels.push_back(static_cast<uint8_t>(pi)); break; }
                    }
                }
            }

            img.pixels = std::move(new_pixels);
            img.color_type = 3;
            img.bit_depth = (palette.size() <= 2) ? 1 :
                            (palette.size() <= 4) ? 2 :
                            (palette.size() <= 16) ? 4 : 8;

            if (!need_alpha)
                img.alpha_palette.clear();
        }
    }

    // Bit depth reduction
    if (img.color_type != 3 && img.bit_depth > 8) {
        bool can_reduce = true;
        for (size_t i = 0; i < img.pixels.size() && can_reduce; i += 2) {
            if (img.pixels[i + 1] != 0) can_reduce = false; // high byte non-zero
        }
        if (can_reduce) {
            // 16-bit -> 8-bit
            std::vector<uint8_t> new_pixels;
            new_pixels.reserve(img.pixels.size() / 2);
            for (size_t i = 0; i < img.pixels.size(); i += 2)
                new_pixels.push_back(img.pixels[i]);
            img.pixels = std::move(new_pixels);
            img.bit_depth = 8;
        }
    }
}

} // namespace fpng

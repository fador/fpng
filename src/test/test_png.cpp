#include "test/test_framework.hpp"
#include "util/crc32.hpp"
#include "png/filter.hpp"
#include "compress/deflate/inflate.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/reader.hpp"
#include "png/writer.hpp"

#include <cstring>
#include <vector>

using namespace fpng;
using namespace fpng::test;

void test_crc32() {
    std::cout << "CRC32 Tests:\n";
    const uint8_t test_data[] = "123456789";
    uint32_t result = CRC32::compute(test_data, 9);
    run_test("CRC32 of '123456789'", result == 0xcbf43926u);
}

void test_filter() {
    std::cout << "Filter Tests:\n";

    const size_t N = 16;
    uint8_t original[N];
    for (size_t i = 0; i < N; ++i) original[i] = static_cast<uint8_t>(i * 17);

    uint8_t prev[N];
    for (size_t i = 0; i < N; ++i) prev[i] = static_cast<uint8_t>((255 - i) * 13);

    for (int ft = 0; ft <= 4; ++ft) {
        FilterType type = static_cast<FilterType>(ft);

        uint8_t filtered[N + 1];
        filter_scanline(type, original, filtered, 1, N, prev);

        uint8_t recon[N] = {};
        unfilter_scanline(type, filtered + 1, recon, 1, N, prev);

        bool ok = (std::memcmp(original, recon, N) == 0);
        std::string name = "Filter ";
        name += (ft == 0 ? "None" : ft == 1 ? "Sub" : ft == 2 ? "Up" :
                 ft == 3 ? "Average" : "Paeth");
        name += " roundtrip";
        run_test(name, ok);
    }
}

void test_inflate() {
    std::cout << "DEFLATE Tests:\n";

    std::vector<uint8_t> test_data;
    for (int i = 0; i < 1000; ++i)
        test_data.push_back(static_cast<uint8_t>(i & 0xff));

    auto compressed = deflate_compress(test_data);
    auto decompressed = inflate_raw(compressed);

    bool ok = (decompressed.size() == test_data.size() &&
               std::memcmp(decompressed.data(), test_data.data(), test_data.size()) == 0);
    run_test("deflate/inflate stored block roundtrip", ok);

    auto zcompressed = zlib_compress(test_data);
    auto zdecompressed = inflate_zlib(zcompressed);
    ok = (zdecompressed.size() == test_data.size() &&
          std::memcmp(zdecompressed.data(), test_data.data(), test_data.size()) == 0);
    run_test("zlib compress/decompress roundtrip", ok);
}

void test_roundtrip() {
    std::cout << "PNG Roundtrip Tests:\n";

    Image img;
    img.width = 4;
    img.height = 4;
    img.bit_depth = 8;
    img.color_type = 6;
    img.interlaced = false;

    size_t row_size = img.raw_scanline_size();
    img.pixels.resize(row_size * img.height);

    for (size_t y = 0; y < img.height; ++y) {
        for (size_t x = 0; x < img.width; ++x) {
            size_t off = y * row_size + x * 4;
            img.pixels[off + 0] = static_cast<uint8_t>(x * 64);
            img.pixels[off + 1] = static_cast<uint8_t>(y * 64);
            img.pixels[off + 2] = static_cast<uint8_t>((x + y) * 32);
            img.pixels[off + 3] = 255;
        }
    }

    PNGWriter writer;
    auto png_data = writer.write(img);

    PNGReader reader;
    auto result = reader.read(png_data);

    bool ok = result.has_value();
    run_test("PNG roundtrip - parse success", ok);

    if (ok) {
        auto& img2 = *result;
        ok = (img2.width == img.width && img2.height == img.height &&
              img2.bit_depth == img.bit_depth && img2.color_type == img.color_type);
        run_test("PNG roundtrip - header match", ok);

        ok = (img2.pixels.size() == img.pixels.size() &&
              std::memcmp(img2.pixels.data(), img.pixels.data(), img.pixels.size()) == 0);
        run_test("PNG roundtrip - pixel data match", ok);
    }

    img.interlaced = true;
    PNGWriter writer2;
    auto png_data2 = writer2.write(img);

    PNGReader reader2;
    auto result2 = reader2.read(png_data2);

    ok = result2.has_value();
    run_test("PNG interlaced roundtrip - parse success", ok);

    if (ok) {
        auto& img3 = *result2;
        ok = (img3.pixels.size() == img.pixels.size() &&
              std::memcmp(img3.pixels.data(), img.pixels.data(), img.pixels.size()) == 0);
        run_test("PNG interlaced roundtrip - pixel data match", ok);
    }
}

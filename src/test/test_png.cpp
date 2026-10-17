#include "test/test_framework.hpp"
#include "util/crc32.hpp"
#include "png/filter.hpp"
#include "compress/deflate/inflate.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/reader.hpp"
#include "png/writer.hpp"
#include "compress/compressor.hpp"

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

void test_full_compress_pipeline() {
    std::cout << "Full Compress Pipeline Tests:\n";

    // A graphic-like image with flat color regions (good LZ77/Huffman targets).
    Image img;
    img.width = 32;
    img.height = 24;
    img.bit_depth = 8;
    img.color_type = 6;
    img.interlaced = false;

    size_t row_size = img.raw_scanline_size();
    img.pixels.resize(row_size * img.height);
    for (size_t y = 0; y < img.height; ++y) {
        for (size_t x = 0; x < img.width; ++x) {
            size_t off = y * row_size + x * 4;
            img.pixels[off + 0] = static_cast<uint8_t>((x / 8) * 64);
            img.pixels[off + 1] = static_cast<uint8_t>((y / 8) * 85);
            img.pixels[off + 2] = static_cast<uint8_t>((x + y) % 3 == 0 ? 255 : 0);
            img.pixels[off + 3] = 255;
        }
    }

    CompressOptions copts;
    copts.level = 5;
    copts.multi_strategy = true;
    copts.num_threads = 2;

    CompressResult res = compress(img, copts);
    bool ok = !res.data.empty();
    run_test("compress() produces output", ok);

    if (ok) {
        PNGReader reader;
        auto result = reader.read(res.data);
        ok = result.has_value();
        run_test("compress() output parses", ok);

        if (ok) {
            // The compressor may losslessly reduce format (e.g. RGBA->RGB when
            // alpha is fully opaque), so compare the visual RGB content rather
            // than raw buffer layout.
            auto& img2 = *result;
            ok = (img2.width == img.width && img2.height == img.height);
            if (ok) {
                for (size_t y = 0; y < img.height && ok; ++y) {
                    for (size_t x = 0; x < img.width; ++x) {
                        size_t src = y * row_size + x * 4;
                        size_t dst = y * img2.raw_scanline_size() + x * img2.bytes_per_pixel();
                        for (int c = 0; c < 3; ++c) {
                            if (img2.pixels[dst + c] != img.pixels[src + c]) { ok = false; break; }
                        }
                    }
                }
            }
            run_test("compress() visual content matches", ok);
        }
    }

    // Test level 9 compression with color reduction + sub-byte packing (4-color image -> 2-bit indexed)
    {
        CompressOptions copts9;
        copts9.level = 9;
        copts9.multi_strategy = true;
        copts9.num_threads = 2;

        CompressResult res9 = compress(img, copts9);
        PNGReader reader;
        auto dec9 = reader.read(res9.data);
        bool ok9 = dec9.has_value() && res9.data.size() <= res.data.size();
        run_test("compress() level 9 <= level 5 size and valid", ok9);
    }

    // Test 8-bit grayscale -> 1-bit grayscale sub-byte packing
    {
        Image gimg;
        gimg.width = 32;
        gimg.height = 32;
        gimg.bit_depth = 8;
        gimg.color_type = 0;
        gimg.pixels.resize(32 * 32);
        for (size_t i = 0; i < gimg.pixels.size(); ++i) {
            size_t x = i % 32, y = i / 32;
            gimg.pixels[i] = ((x * 7 + y * 13 + (x ^ y)) % 5 < 2) ? 0 : 255;
        }
        CompressOptions copts9;
        copts9.level = 9;
        CompressResult gres = compress(gimg, copts9);
        PNGReader reader;
        auto gdec = reader.read(gres.data);
        bool gok = gdec.has_value() && gdec->width == 32 && gdec->height == 32 && gdec->bit_depth == 1;
        run_test("8-bit binary grayscale packs to 1-bit grayscale", gok);
    }

    // Test 8-bit indexed with duplicate/unused entries -> compacted sub-byte indexed
    {
        Image pimg;
        pimg.width = 16;
        pimg.height = 16;
        pimg.bit_depth = 8;
        pimg.color_type = 3;
        // 10 palette entries, only indices 2, 5, 7 used, where 2 and 7 are identical RGB
        pimg.palette.resize(30, 0);
        pimg.palette[2 * 3 + 0] = 255; pimg.palette[2 * 3 + 1] = 0;   pimg.palette[2 * 3 + 2] = 0;
        pimg.palette[5 * 3 + 0] = 0;   pimg.palette[5 * 3 + 1] = 255; pimg.palette[5 * 3 + 2] = 0;
        pimg.palette[7 * 3 + 0] = 255; pimg.palette[7 * 3 + 1] = 0;   pimg.palette[7 * 3 + 2] = 0;
        pimg.pixels.resize(16 * 16);
        for (size_t i = 0; i < pimg.pixels.size(); ++i) {
            pimg.pixels[i] = (i % 3 == 0) ? 2 : ((i % 3 == 1) ? 5 : 7);
        }
        CompressOptions copts9;
        copts9.level = 9;
        CompressResult pres = compress(pimg, copts9);
        PNGReader reader;
        auto pdec = reader.read(pres.data);
        bool pok = pdec.has_value() && pdec->bit_depth == 1 && pdec->palette.size() == 6;
        run_test("8-bit indexed compacts duplicates/unused to 1-bit 2-color PLTE", pok);
    }
}

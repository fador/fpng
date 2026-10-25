// Regression tests for real-world / malformed input handling.
#include "test/test_framework.hpp"
#include "util/crc32.hpp"
#include "util/endian.hpp"
#include "compress/deflate/inflate.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/reader.hpp"
#include "png/writer.hpp"
#include "compress/compressor.hpp"
#include "image/compare.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace fpng;
using namespace fpng::test;

namespace {

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& d,
               bool bad_crc = false) {
    uint8_t b[4];
    write_big32(b, static_cast<uint32_t>(d.size()));
    out.insert(out.end(), b, b + 4);
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), d.begin(), d.end());
    CRC32 c;
    c.update(reinterpret_cast<const uint8_t*>(type), 4);
    if (!d.empty()) c.update(d.data(), d.size());
    write_big32(b, c.finalize() ^ (bad_crc ? 1u : 0u));
    out.insert(out.end(), b, b + 4);
}

std::vector<uint8_t> ihdr(uint32_t w, uint32_t h, uint8_t bd, uint8_t ct, uint8_t il = 0) {
    std::vector<uint8_t> d(13);
    write_big32(d.data(), w);
    write_big32(d.data() + 4, h);
    d[8] = bd; d[9] = ct; d[12] = il;
    return d;
}

std::vector<uint8_t> zlib_of(const std::vector<uint8_t>& raw) {
    DeflateOptions o;
    return zlib_compress(raw, o);
}

// 4x2 RGB image, filter byte `ft` on every row.
std::vector<uint8_t> small_rgb_png(uint8_t ft, bool with_iend = true,
                                   const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extra = {},
                                   bool extra_bad_crc = false) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < 2; ++y) {
        raw.push_back(ft);
        for (int x = 0; x < 4; ++x) { raw.push_back(uint8_t(x * 60)); raw.push_back(uint8_t(y * 90)); raw.push_back(7); }
    }
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    put_chunk(png, "IHDR", ihdr(4, 2, 8, 2));
    for (auto& [t, d] : extra) put_chunk(png, t.c_str(), d, extra_bad_crc);
    put_chunk(png, "IDAT", zlib_of(raw));
    if (with_iend) put_chunk(png, "IEND", {});
    return png;
}

Image make_rgba(uint32_t w, uint32_t h, bool opaque) {
    Image img;
    img.width = w; img.height = h; img.bit_depth = 8; img.color_type = 6;
    img.pixels.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        img.pixels[i * 4 + 0] = uint8_t(i * 7);
        img.pixels[i * 4 + 1] = uint8_t(i * 13);
        img.pixels[i * 4 + 2] = uint8_t(i * 29);
        img.pixels[i * 4 + 3] = opaque ? 255 : uint8_t(i * 31);
    }
    return img;
}

bool compress_and_verify(const Image& img, int level, std::vector<uint8_t>* out = nullptr) {
    CompressOptions o;
    o.level = level;
    o.num_threads = 2;
    auto r = compress(img, o);
    PNGReader rd;
    auto back = rd.read(r.data);
    if (!back || !rd.warnings().empty()) return false;
    if (out) *out = r.data;
    return images_equivalent(img, *back);
}

bool contains_chunk(const std::vector<uint8_t>& png, const char* type, size_t* len = nullptr) {
    size_t off = 8;
    while (off + 12 <= png.size()) {
        uint32_t l = read_big32(png.data() + off);
        if (std::memcmp(png.data() + off + 4, type, 4) == 0) {
            if (len) *len = l;
            return true;
        }
        off += 12 + l;
    }
    return false;
}

} // namespace

void test_robustness() {
    std::cout << "Robustness Tests:\n";

    // --- inflate hardening ---
    {
        // Dynamic block header with HLIT = 288 (invalid; used to overflow a stack array).
        std::vector<uint8_t> bad = {0x78, 0x9c, 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
        bool threw = false;
        try { inflate_zlib(bad); } catch (const std::exception&) { threw = true; }
        run_test("inflate rejects HLIT > 286", threw);

        std::vector<uint8_t> data(5000, 'a');
        auto z = zlib_of(data);
        threw = false;
        try { inflate_zlib(z, 1000); } catch (const std::exception&) { threw = true; }
        run_test("inflate enforces output limit (zip-bomb guard)", threw);

        auto z2 = z;
        z2.back() ^= 0x55;
        threw = false;
        try { inflate_zlib(z2); } catch (const std::exception&) { threw = true; }
        run_test("inflate detects Adler-32 mismatch", threw);

        auto z3 = z;
        z3.insert(z3.end(), {0, 0, 0, 0, 0});
        bool ok = false;
        try { ok = inflate_zlib(z3) == data; } catch (...) {}
        run_test("inflate tolerates padding after zlib stream", ok);
    }

    // --- reader hardening ---
    {
        PNGReader r;
        run_test("reader rejects invalid filter type", !r.read(small_rgb_png(7)));

        PNGReader r2;
        auto img = r2.read(small_rgb_png(1, /*with_iend=*/false));
        run_test("reader accepts file truncated after IDAT (missing IEND)", img.has_value());

        PNGReader r3;
        auto img3 = r3.read(small_rgb_png(0, true, {{"tEXt", {'a', 0, 'b'}}}, /*bad crc*/ true));
        run_test("reader drops ancillary chunk with bad CRC", img3.has_value() && img3->ancillary.text_chunks.empty());

        PNGReader r4;
        run_test("reader rejects unknown critical chunk",
                 !r4.read(small_rgb_png(0, true, {{"ABCD", {1, 2, 3}}})));

        PNGReader r5;
        auto img5 = r5.read(small_rgb_png(0, true, {{"eXIf", {'M', 'M', 0, 42, 0, 0, 0, 8}}, {"prVt", {1}}}));
        run_test("reader keeps eXIf and safe-to-copy chunks",
                 img5 && img5->ancillary.other_chunks.size() == 2);

        PNGReader r6;
        auto png6 = small_rgb_png(0);
        png6.insert(png6.end(), {'j', 'u', 'n', 'k', 0, 0, 0});
        run_test("reader ignores garbage after IEND", r6.read(png6).has_value());

        // Hostile dimensions must fail cleanly, not allocate terabytes.
        std::vector<uint8_t> huge = {137, 80, 78, 71, 13, 10, 26, 10};
        put_chunk(huge, "IHDR", ihdr(0x7fffffff, 0x7fffffff, 16, 6));
        put_chunk(huge, "IDAT", zlib_of({0}));
        put_chunk(huge, "IEND", {});
        PNGReader r7;
        run_test("reader rejects absurd dimensions", !r7.read(huge));

        // Palette index beyond PLTE: palette gets padded so output is valid.
        std::vector<uint8_t> pal = {137, 80, 78, 71, 13, 10, 26, 10};
        put_chunk(pal, "IHDR", ihdr(2, 1, 8, 3));
        put_chunk(pal, "PLTE", {255, 0, 0});
        put_chunk(pal, "IDAT", zlib_of({0, 0, 5}));
        put_chunk(pal, "IEND", {});
        PNGReader r8;
        auto img8 = r8.read(pal);
        run_test("out-of-range palette index padded", img8 && img8->palette.size() == 18 &&
                                                        compress_and_verify(*img8, 2));
    }

    // --- writer: colour-dependent chunks after reduction ---
    {
        Image img = make_rgba(16, 16, /*opaque=*/true);   // alpha gets stripped -> RGB
        img.ancillary.sBIT = std::vector<uint8_t>{8, 8, 8, 8};
        img.ancillary.bKGD = std::vector<uint8_t>{0, 255, 0, 255, 0, 255};
        std::vector<uint8_t> out;
        bool ok = compress_and_verify(img, 2, &out);
        size_t sbit_len = 0;
        bool has = contains_chunk(out, "sBIT", &sbit_len);
        run_test("sBIT rewritten to 3 bytes after RGBA->RGB", ok && has && sbit_len == 3);

        Image g = make_rgba(8, 8, true);
        for (size_t i = 0; i < 64; ++i) g.pixels[i * 4 + 1] = g.pixels[i * 4 + 2] = g.pixels[i * 4];
        g.ancillary.sBIT = std::vector<uint8_t>{5, 6, 5, 8};
        out.clear();
        ok = compress_and_verify(g, 9, &out);
        has = contains_chunk(out, "sBIT", &sbit_len);
        run_test("sBIT valid after RGBA->gray reduction", ok && (!has || sbit_len == 1));
    }

    // --- APNG round trip (frame 0 = IDAT, plus 2 sub-frames) ---
    {
        Image a = make_rgba(8, 8, false);
        a.is_animated = true;
        a.num_plays = 0;
        FrameInfo f0;
        f0.width = 8; f0.height = 8; f0.delay_num = 1; f0.delay_den = 10; f0.from_idat = true;
        a.frames.push_back(f0);
        for (int k = 0; k < 2; ++k) {
            FrameInfo f;
            f.width = 4; f.height = 3; f.x_offset = 2; f.y_offset = uint32_t(k);
            f.delay_num = 2; f.delay_den = 10; f.blend_op = 1;
            f.image_data.resize(4 * 3 * 4);
            for (size_t i = 0; i < f.image_data.size(); ++i) f.image_data[i] = uint8_t(i * (k + 3));
            a.frames.push_back(f);
        }
        std::vector<uint8_t> out;
        run_test("APNG round-trips (frames, timing, pixels)", compress_and_verify(a, 9, &out));
    }

    // --- interlaced input of awkward sizes ---
    {
        bool all = true;
        for (uint32_t w : {1u, 2u, 3u, 5u, 9u}) {
            for (uint32_t h : {1u, 4u, 7u}) {
                Image img = make_rgba(w, h, false);
                PNGWriter wr;
                WriteOptions wo;
                wo.interlace = true;
                auto png = wr.write(img, wo);
                PNGReader rd;
                auto back = rd.read(png);
                if (!back || !images_equivalent(img, *back) || !compress_and_verify(*back, 1)) all = false;
            }
        }
        run_test("Adam7 small sizes decode and re-encode", all);
    }
}

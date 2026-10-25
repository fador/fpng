#include "png/writer.hpp"
#include "png/chunk.hpp"
#include "png/filter.hpp"
#include "png/ihdr.hpp"
#include "compress/deflate/deflater.hpp"
#include "compress/filter_optimizer.hpp"
#include "util/crc32.hpp"
#include "util/endian.hpp"
#include "util/file.hpp"

#include <cstring>
#include <climits>
#include <algorithm>
#include <stdexcept>

namespace fpng {

static const uint8_t PNG_SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

void PNGWriter::write_chunk(std::vector<uint8_t>& out,
                             const char type[4],
                             std::span<const uint8_t> data) {
    // Length
    uint8_t len_buf[4];
    write_big32(len_buf, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), len_buf, len_buf + 4);

    // Type
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(type),
               reinterpret_cast<const uint8_t*>(type) + 4);

    // Data
    if (!data.empty())
        out.insert(out.end(), data.begin(), data.end());

    // CRC (over type + data)
    CRC32 crc;
    crc.update(reinterpret_cast<const uint8_t*>(type), 4);
    if (!data.empty())
        crc.update(data.data(), data.size());
    uint32_t crc_val = crc.finalize();

    uint8_t crc_buf[4];
    write_big32(crc_buf, crc_val);
    out.insert(out.end(), crc_buf, crc_buf + 4);
}

void PNGWriter::write_signature(std::vector<uint8_t>& out) {
    out.insert(out.end(), PNG_SIG, PNG_SIG + 8);
}

void PNGWriter::write_ihdr(std::vector<uint8_t>& out, const Image& img,
                           const WriteOptions& wopts) {
    uint8_t ihdr[13];
    write_big32(ihdr, img.width);
    write_big32(ihdr + 4, img.height);
    ihdr[8] = img.bit_depth;
    ihdr[9] = img.color_type;
    ihdr[10] = 0; // compression method = deflate
    ihdr[11] = 0; // filter method = adaptive
    ihdr[12] = wopts.interlace ? 1 : 0;

    write_chunk(out, "IHDR", {ihdr, 13});
}

void PNGWriter::write_plte(std::vector<uint8_t>& out, const Image& img) {
    if (!img.palette.empty()) {
        write_chunk(out, "PLTE", img.palette);
    }
}

void PNGWriter::write_trns(std::vector<uint8_t>& out, const Image& img) {
    if (img.color_type == 3) {
        if (!img.alpha_palette.empty()) {
            // PNG allows a tRNS shorter than the palette: trailing entries
            // default to opaque (255). Trim them to save raw chunk bytes.
            size_t n = img.alpha_palette.size();
            while (n > 0 && img.alpha_palette[n - 1] == 255) --n;
            if (n > 0) {
                write_chunk(out, "tRNS",
                            {img.alpha_palette.data(), n});
            }
        }
    } else if (!img.ancillary.tRNS.empty()) {
        write_chunk(out, "tRNS", img.ancillary.tRNS);
    }
}

namespace {

size_t channels_of(uint8_t ct) {
    switch (ct) { case 0: return 1; case 2: return 3; case 3: return 3; case 4: return 2; default: return 4; }
}

// Make colour-dependent ancillary chunks consistent with the (possibly
// reduced) colour type / bit depth / palette, converting where possible and
// dropping them where not. Writing them unchanged produced invalid files
// (e.g. a 4-byte sBIT on an RGB image after alpha stripping).
void sanitize_for_format(Image& img) {
    auto& a = img.ancillary;
    const uint8_t ct = img.color_type;
    const unsigned sample_depth = (ct == 3) ? 8u : img.bit_depth;

    if (ct == 0 || ct == 4) img.palette.clear();  // PLTE forbidden for grayscale
    if (ct == 3) {
        size_t n = img.palette.size() / 3;
        if (img.alpha_palette.size() > n) img.alpha_palette.resize(n);
    } else {
        img.alpha_palette.clear();
    }

    // tRNS for grayscale / truecolour: exact length and in-range value.
    if (!a.tRNS.empty()) {
        size_t want = (ct == 0) ? 2 : (ct == 2) ? 6 : 0;
        bool ok = want != 0 && a.tRNS.size() == want;
        if (ok && img.bit_depth < 16) {
            unsigned maxv = (1u << img.bit_depth) - 1;
            for (size_t i = 0; i < want; i += 2)
                if (static_cast<unsigned>((a.tRNS[i] << 8) | a.tRNS[i + 1]) > maxv) ok = false;
        }
        if (!ok) a.tRNS.clear();
    }

    // sBIT: rebuild for the current channel layout from whatever layout the
    // stored values describe (1=G, 2=GA, 3=RGB, 4=RGBA).
    if (a.sBIT) {
        const auto& old = *a.sBIT;
        bool ok = old.size() >= 1 && old.size() <= 4;
        unsigned color_sig = 0, alpha_sig = 0;
        if (ok) {
            size_t nc = (old.size() >= 3) ? 3 : 1;
            for (size_t i = 0; i < nc; ++i) color_sig = std::max<unsigned>(color_sig, old[i]);
            if (old.size() == 2) alpha_sig = old[1];
            if (old.size() == 4) alpha_sig = old[3];
        }
        size_t want = channels_of(ct);
        bool has_alpha = (ct == 4 || ct == 6);
        if (ok && color_sig > 0 && (!has_alpha || alpha_sig > 0)) {
            std::vector<uint8_t> nb;
            size_t ncolor = (ct == 0 || ct == 4) ? 1 : 3;
            for (size_t i = 0; i < ncolor; ++i) {
                unsigned v = (old.size() >= 3 && ncolor == 3) ? old[i] : color_sig;
                nb.push_back(static_cast<uint8_t>(std::clamp(v, 1u, sample_depth)));
            }
            if (has_alpha) nb.push_back(static_cast<uint8_t>(std::clamp(alpha_sig, 1u, sample_depth)));
            if (nb.size() == want) a.sBIT = std::move(nb); else a.sBIT.reset();
        } else {
            a.sBIT.reset();
        }
    }

    // bKGD: must match the colour type; values must be representable.
    if (a.bKGD) {
        const auto& b = *a.bKGD;
        bool ok;
        if (ct == 3) {
            ok = b.size() == 1 && b[0] < img.palette.size() / 3;
        } else {
            size_t want = (ct == 0 || ct == 4) ? 2 : 6;
            ok = b.size() == want;
            if (ok && img.bit_depth < 16) {
                unsigned maxv = (1u << img.bit_depth) - 1;
                for (size_t i = 0; i < want; i += 2)
                    if (static_cast<unsigned>((b[i] << 8) | b[i + 1]) > maxv) ok = false;
            }
        }
        if (!ok) a.bKGD.reset();
    }
}

} // namespace

void PNGWriter::write_ancillary(std::vector<uint8_t>& out, const Image& img) {
    // Chunks that must precede PLTE.
    const auto& a = img.ancillary;
    auto write_if = [&](const char* tag, const std::optional<std::vector<uint8_t>>& data) {
        if (data) write_chunk(out, tag, *data);
    };
    write_if("cICP", a.cICP);
    write_if("gAMA", a.gAMA);
    write_if("cHRM", a.cHRM);
    // iCCP and sRGB are mutually exclusive per spec; prefer the explicit profile.
    if (a.iCCP) write_if("iCCP", a.iCCP);
    else write_if("sRGB", a.sRGB);
    write_if("sBIT", a.sBIT);
    write_other(out, img, 0);
}

void PNGWriter::write_post_plte(std::vector<uint8_t>& out, const Image& img) {
    // Chunks that must follow PLTE (bKGD, tRNS) or merely precede IDAT.
    const auto& a = img.ancillary;
    write_trns(out, img);
    if (a.bKGD) write_chunk(out, "bKGD", *a.bKGD);
    if (a.pHYs) write_chunk(out, "pHYs", *a.pHYs);
    if (a.tIME) write_chunk(out, "tIME", *a.tIME);
    for (const auto& [tag, data] : a.text_chunks)
        write_chunk(out, tag.c_str(), data);
    write_other(out, img, 1);
}

void PNGWriter::write_trailing(std::vector<uint8_t>& out, const Image& img) {
    write_other(out, img, 2);
}

void PNGWriter::write_other(std::vector<uint8_t>& out, const Image& img, uint8_t where) {
    for (const auto& c : img.ancillary.other_chunks)
        if (c.where == where && c.type.size() == 4)
            write_chunk(out, c.type.c_str(), c.data);
}

static void put_fctl(uint8_t* fctl, const FrameInfo& f, uint32_t seq) {
    write_big32(fctl, seq);
    write_big32(fctl + 4, f.width);
    write_big32(fctl + 8, f.height);
    write_big32(fctl + 12, f.x_offset);
    write_big32(fctl + 16, f.y_offset);
    write_big16(fctl + 20, f.delay_num);
    write_big16(fctl + 22, f.delay_den);
    fctl[24] = f.dispose_op;
    fctl[25] = f.blend_op;
}

void PNGWriter::write_apng_chunks(std::vector<uint8_t>& out, const Image& img) {
    if (!img.is_animated || img.frames.empty()) return;

    uint8_t actl[8];
    write_big32(actl, static_cast<uint32_t>(img.frames.size()));
    write_big32(actl + 4, img.num_plays);
    write_chunk(out, "acTL", {actl, 8});
}

void PNGWriter::write_data_chunks(std::vector<uint8_t>& out, const char type[4],
                                  const std::vector<uint8_t>& prefix,
                                  const std::vector<uint8_t>& payload) {
    // Chunk lengths are limited to 2^31-1; split very large streams.
    const size_t kMax = size_t(1) << 30;
    size_t pos = 0;
    do {
        size_t n = std::min(kMax, payload.size() - pos);
        if (prefix.empty()) {
            write_chunk(out, type, {payload.data() + pos, n});
        } else {
            std::vector<uint8_t> buf(prefix);
            buf.insert(buf.end(), payload.begin() + pos, payload.begin() + pos + n);
            write_chunk(out, type, buf);
        }
        pos += n;
    } while (pos < payload.size());
}

void PNGWriter::write_idat(std::vector<uint8_t>& out, const Image& img,
                             const WriteOptions& wopts) {
    // Sequence numbers are rewritten densely from 0 (fcTL and fdAT share them).
    uint32_t seq = 0;
    bool animated = img.is_animated && !img.frames.empty();

    if (animated && img.frames[0].from_idat) {
        uint8_t fctl[26];
        put_fctl(fctl, img.frames[0], seq++);
        write_chunk(out, "fcTL", {fctl, 26});
    }

    auto compressed = filter_and_compress(img, wopts);
    if (compressed.empty()) throw std::runtime_error("PNG writer: image compression produced no data");
    write_data_chunks(out, "IDAT", {}, compressed);

    if (!animated) return;
    for (size_t i = 0; i < img.frames.size(); ++i) {
        const auto& f = img.frames[i];
        if (f.from_idat) continue;

        uint8_t fctl[26];
        put_fctl(fctl, f, seq++);
        write_chunk(out, "fcTL", {fctl, 26});

        auto frame_compressed = filter_and_compress_frame(img, f, wopts);
        if (frame_compressed.empty()) throw std::runtime_error("PNG writer: frame compression produced no data");
        // fdAT payload = sequence number + zlib data; each split chunk gets its own number.
        const size_t kMax = size_t(1) << 30;
        size_t pos = 0;
        do {
            size_t n = std::min(kMax, frame_compressed.size() - pos);
            std::vector<uint8_t> fdat(4);
            write_big32(fdat.data(), seq++);
            fdat.insert(fdat.end(), frame_compressed.begin() + pos, frame_compressed.begin() + pos + n);
            write_chunk(out, "fdAT", fdat);
            pos += n;
        } while (pos < frame_compressed.size());
    }
}

void PNGWriter::write_iend(std::vector<uint8_t>& out) {
    write_chunk(out, "IEND", {});
}

std::vector<uint8_t> PNGWriter::filter_and_compress(const Image& img,
                                                    const WriteOptions& wopts) {
    if (img.pixels.empty()) return {};

    size_t raw_ss = img.raw_scanline_size();
    size_t bpp = img.bytes_per_pixel();
    size_t height = img.height;

    std::vector<uint8_t> filtered;
    filtered.reserve((raw_ss + 1) * height);

    // Get filters: use provided ones if available, else auto-compute
    std::vector<FilterType> filters = wopts.filters;
    if (filters.empty()) {
        FilterOptions fopts;
        fopts.level = 2;
        filters = optimize_filters(img, fopts);
    }

    if (wopts.interlace) {
        struct { uint32_t x0, y0, dx, dy; } passes[7] = {
            {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4}, {0,2,2,4}, {1,0,2,2}, {0,1,1,2}
        };

        for (int pass = 0; pass < 7; ++pass) {
            auto& p = passes[pass];
            uint32_t pw = (img.width + p.dx - 1 - p.x0) / p.dx;
            uint32_t ph = (img.height + p.dy - 1 - p.y0) / p.dy;
            // Fix for edge cases - compute properly
            if (p.x0 >= img.width) pw = 0;
            if (p.y0 >= img.height) ph = 0;
            if (pw == 0 || ph == 0) continue;

            size_t pass_stride = img.scanline_size_for_width(pw);

            std::vector<uint8_t> prev_row(pass_stride, 0);
            std::vector<uint8_t> row_buf(1 + pass_stride);

            for (uint32_t py = 0; py < ph; ++py) {
                uint32_t img_y = p.y0 + py * p.dy;
                if (img_y >= img.height) continue;

                const uint8_t* src_row = img.pixels.data() + img_y * raw_ss;

                // Extract this pass's pixels from the source row
                std::vector<uint8_t> pass_row(pass_stride, 0);
                if (img.bit_depth < 8) {
                    // Bit-packed indexed or grayscale
                    for (uint32_t px = 0; px < pw; ++px) {
                        uint32_t img_x = p.x0 + px * p.dx;
                        if (img_x >= img.width) continue;
                        size_t src_byte = static_cast<size_t>(img_x) * img.bit_depth / 8;
                        size_t dst_byte = static_cast<size_t>(px) * img.bit_depth / 8;
                        int src_bit = static_cast<int>((img_x * img.bit_depth) % 8);
                        int dst_bit = static_cast<int>((px * img.bit_depth) % 8);
                        uint8_t val = (src_row[src_byte] >> (8 - img.bit_depth - src_bit)) &
                                      ((1u << img.bit_depth) - 1);
                        pass_row[dst_byte] |= static_cast<uint8_t>(val << (8 - img.bit_depth - dst_bit));
                    }
                } else {
                    for (uint32_t px = 0; px < pw; ++px) {
                        uint32_t img_x = p.x0 + px * p.dx;
                        if (img_x >= img.width) continue;
                        size_t src_off = static_cast<size_t>(img_x) * bpp;
                        size_t dst_off = static_cast<size_t>(px) * bpp;
                        for (size_t b = 0; b < bpp; ++b)
                            pass_row[dst_off + b] = src_row[src_off + b];
                    }
                }

                filter_scanline(FilterType::None, pass_row.data(), row_buf.data(),
                               bpp, pass_stride, py > 0 ? prev_row.data() : nullptr);
                filtered.insert(filtered.end(), row_buf.data(), row_buf.data() + 1 + pass_stride);

                std::swap(prev_row, pass_row);
            }
        }
    } else {
        std::vector<uint8_t> prev_scanline(raw_ss, 0);
        std::vector<uint8_t> row_buf(1 + raw_ss);
        for (size_t y = 0; y < height; ++y) {
            const uint8_t* src = img.pixels.data() + y * raw_ss;
            FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
            filter_scanline(ft, src, row_buf.data(), bpp, raw_ss,
                             y > 0 ? prev_scanline.data() : nullptr);
            filtered.insert(filtered.end(), row_buf.begin(), row_buf.end());
            std::memcpy(prev_scanline.data(), src, raw_ss);
        }
    }

    DeflateOptions deflate = wopts.deflate;
    if (deflate.row_stride == 0 && raw_ss + 1 <= INT_MAX)
        deflate.row_stride = static_cast<int>(raw_ss + 1);
    return zlib_compress(filtered, deflate);
}

std::vector<uint8_t> PNGWriter::filter_and_compress_frame(const Image& img,
                                                            const FrameInfo& finfo,
                                                            const WriteOptions& wopts) {
    if (finfo.image_data.empty()) return {};

    size_t bpp = img.bytes_per_pixel();
    size_t raw_ss = img.scanline_size_for_width(finfo.width);
    size_t height = finfo.height;

    Image frame_img;
    frame_img.width = finfo.width;
    frame_img.height = finfo.height;
    frame_img.bit_depth = img.bit_depth;
    frame_img.color_type = img.color_type;
    frame_img.pixels = finfo.image_data;

    FilterOptions fopts;
    fopts.level = 2;
    auto filters = optimize_filters(frame_img, fopts);

    std::vector<uint8_t> filtered;
    filtered.reserve((raw_ss + 1) * height);

    std::vector<uint8_t> prev_scanline(raw_ss, 0);
    std::vector<uint8_t> row_buf(1 + raw_ss);

    for (size_t y = 0; y < height; ++y) {
        const uint8_t* src = finfo.image_data.data() + y * raw_ss;
        FilterType ft = (y < filters.size()) ? filters[y] : FilterType::None;
        filter_scanline(ft, src, row_buf.data(), bpp, raw_ss,
                         y > 0 ? prev_scanline.data() : nullptr);
        filtered.insert(filtered.end(), row_buf.begin(), row_buf.end());
        std::memcpy(prev_scanline.data(), src, raw_ss);
    }

    DeflateOptions deflate = wopts.deflate;
    if (raw_ss + 1 <= INT_MAX) deflate.row_stride = static_cast<int>(raw_ss + 1);
    return zlib_compress(filtered, deflate);
}

std::vector<uint8_t> PNGWriter::write(const Image& img_in,
                                       const WriteOptions& wopts) {
    // Work on a header-only copy for chunk sanitising; pixel buffers are
    // shared by reference through `img_in` for compression.
    Image hdr;
    hdr.width = img_in.width;
    hdr.height = img_in.height;
    hdr.bit_depth = img_in.bit_depth;
    hdr.color_type = img_in.color_type;
    hdr.palette = img_in.palette;
    hdr.alpha_palette = img_in.alpha_palette;
    hdr.ancillary = img_in.ancillary;
    hdr.is_animated = img_in.is_animated;
    hdr.num_plays = img_in.num_plays;
    sanitize_for_format(hdr);

    std::vector<uint8_t> out;
    write_signature(out);
    write_ihdr(out, hdr, wopts);
    write_ancillary(out, hdr);
    write_apng_chunks(out, img_in);
    write_plte(out, hdr);
    write_post_plte(out, hdr);
    write_idat(out, img_in, wopts);
    write_trailing(out, hdr);
    write_iend(out);
    return out;
}

bool PNGWriter::write(const std::filesystem::path& path, const Image& img,
                       const WriteOptions& wopts) {
    auto data = write(img, wopts);
    return write_file(path, data);
}

} // namespace fpng

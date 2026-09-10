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

void PNGWriter::write_ihdr(std::vector<uint8_t>& out, const Image& img) {
    uint8_t ihdr[13];
    write_big32(ihdr, img.width);
    write_big32(ihdr + 4, img.height);
    ihdr[8] = img.bit_depth;
    ihdr[9] = img.color_type;
    ihdr[10] = 0; // compression method = deflate
    ihdr[11] = 0; // filter method = adaptive
    ihdr[12] = img.interlaced ? 1 : 0;

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
            write_chunk(out, "tRNS", img.alpha_palette);
        }
    } else if (!img.ancillary.tRNS.empty()) {
        write_chunk(out, "tRNS", img.ancillary.tRNS);
    }
}

void PNGWriter::write_ancillary(std::vector<uint8_t>& out, const Image& img) {
    const auto& a = img.ancillary;
    auto write_if = [&](const char* tag, const std::optional<std::vector<uint8_t>>& data) {
        if (data) write_chunk(out, tag, *data);
    };
    write_if("gAMA", a.gAMA);
    write_if("cHRM", a.cHRM);
    write_if("sRGB", a.sRGB);
    write_if("iCCP", a.iCCP);
    write_if("sBIT", a.sBIT);
    write_if("bKGD", a.bKGD);
    write_if("pHYs", a.pHYs);
    write_if("tIME", a.tIME);
    write_if("cICP", a.cICP);
    for (const auto& [tag, data] : a.text_chunks) {
        write_chunk(out, tag.c_str(), data);
    }
}

void PNGWriter::write_apng_chunks(std::vector<uint8_t>& out, const Image& img) {
    if (!img.is_animated || img.frames.empty()) return;

    // acTL
    uint8_t actl[8];
    write_big32(actl, static_cast<uint32_t>(img.frames.size()));
    write_big32(actl + 4, img.num_plays);
    write_chunk(out, "acTL", {actl, 8});

    // fcTL for frame 0 (the static image / IDAT)
    if (!img.frames.empty()) {
        const auto& f = img.frames[0];
        uint8_t fctl[26];
        write_big32(fctl, f.sequence_number);
        write_big32(fctl + 4, f.width);
        write_big32(fctl + 8, f.height);
        write_big32(fctl + 12, f.x_offset);
        write_big32(fctl + 16, f.y_offset);
        write_big16(fctl + 20, f.delay_num);
        write_big16(fctl + 22, f.delay_den);
        fctl[24] = f.dispose_op;
        fctl[25] = f.blend_op;
        write_chunk(out, "fcTL", {fctl, 26});
    }
}

void PNGWriter::write_idat(std::vector<uint8_t>& out, const Image& img,
                             const WriteOptions& wopts) {
    auto compressed = filter_and_compress(img, wopts);
    write_chunk(out, "IDAT", compressed);

    // Write additional frames as fdAT chunks
    if (img.is_animated && img.frames.size() > 1) {
        for (size_t i = 1; i < img.frames.size(); ++i) {
            const auto& f = img.frames[i];

            // fcTL
            uint8_t fctl[26];
            write_big32(fctl, f.sequence_number);
            write_big32(fctl + 4, f.width);
            write_big32(fctl + 8, f.height);
            write_big32(fctl + 12, f.x_offset);
            write_big32(fctl + 16, f.y_offset);
            write_big16(fctl + 20, f.delay_num);
            write_big16(fctl + 22, f.delay_den);
            fctl[24] = f.dispose_op;
            fctl[25] = f.blend_op;
            write_chunk(out, "fcTL", {fctl, 26});

            // fdAT
            auto frame_compressed = filter_and_compress_frame(img, f);
            std::vector<uint8_t> fdat;
            fdat.reserve(4 + frame_compressed.size());
            uint8_t seq_buf[4];
            write_big32(seq_buf, f.sequence_number);
            fdat.insert(fdat.end(), seq_buf, seq_buf + 4);
            fdat.insert(fdat.end(), frame_compressed.begin(), frame_compressed.end());
            write_chunk(out, "fdAT", fdat);
        }
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

    if (img.interlaced) {
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
                                                            const FrameInfo& finfo) {
    if (finfo.image_data.empty()) return {};

    size_t bpp = img.bytes_per_pixel();
    size_t raw_ss = img.scanline_size_for_width(finfo.width);
    size_t height = finfo.height;

    std::vector<uint8_t> filtered;
    filtered.reserve((raw_ss + 1) * height);

    std::vector<uint8_t> prev_scanline(raw_ss, 0);
    std::vector<uint8_t> row_buf(1 + raw_ss);

    for (size_t y = 0; y < height; ++y) {
        const uint8_t* src = finfo.image_data.data() + y * raw_ss;
        filter_scanline(FilterType::None, src, row_buf.data(), bpp, raw_ss,
                         y > 0 ? prev_scanline.data() : nullptr);
        filtered.insert(filtered.end(), row_buf.begin(), row_buf.end());
        std::memcpy(prev_scanline.data(), src, raw_ss);
    }

    DeflateOptions deflate;
    if (raw_ss + 1 <= INT_MAX) deflate.row_stride = static_cast<int>(raw_ss + 1);
    return zlib_compress(filtered, deflate);
}

std::vector<uint8_t> PNGWriter::write(const Image& img,
                                       const WriteOptions& wopts) {
    std::vector<uint8_t> out;
    write_signature(out);
    write_ihdr(out, img);
    write_ancillary(out, img);
    write_plte(out, img);
    write_trns(out, img);
    write_apng_chunks(out, img);
    write_idat(out, img, wopts);
    write_iend(out);
    return out;
}

bool PNGWriter::write(const std::filesystem::path& path, const Image& img,
                       const WriteOptions& wopts) {
    auto data = write(img, wopts);
    return write_file(path, data);
}

} // namespace fpng

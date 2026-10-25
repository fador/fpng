#include "png/reader.hpp"
#include "png/chunk.hpp"
#include "png/filter.hpp"
#include "png/ihdr.hpp"
#include "compress/deflate/inflate.hpp"
#include "util/crc32.hpp"
#include "util/endian.hpp"
#include "util/file.hpp"

#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace fpng {

static const uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// Adam7 pass geometry. Computed per image into a local table (the previous
// implementation used a mutable global, which raced when several threads
// decoded images concurrently).
struct Adam7Pass {
    uint32_t x0, y0, dx, dy;
    uint32_t width, height;
};

static void compute_adam7(uint32_t w, uint32_t h, Adam7Pass (&p)[7]) {
    static const uint32_t g[7][4] = {
        {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4}, {0,2,2,4}, {1,0,2,2}, {0,1,1,2}};
    for (int i = 0; i < 7; ++i) {
        p[i] = {g[i][0], g[i][1], g[i][2], g[i][3], 0, 0};
        p[i].width  = (w > g[i][0]) ? (w - g[i][0] + g[i][2] - 1) / g[i][2] : 0;
        p[i].height = (h > g[i][1]) ? (h - g[i][1] + g[i][3] - 1) / g[i][3] : 0;
    }
}

// Upper bound on the decoded image buffer (bytes). Keeps hostile or corrupt
// headers (e.g. 2^31 x 2^31) from exhausting memory; real images fit easily.
static constexpr uint64_t MAX_RAW_IMAGE_BYTES = 4ull << 30; // 4 GiB

bool PNGReader::verify_crc(const ChunkInfo& chunk) const {
    // CRC is over chunk type + chunk data
    CRC32 crc;
    crc.update(reinterpret_cast<const uint8_t*>(chunk.type), 4);
    crc.update(chunk.data, chunk.length);
    return crc.finalize() == chunk.crc;
}

std::optional<Image> PNGReader::read(const std::filesystem::path& path) {
    auto data = read_file(path);
    if (data.empty()) {
        error_ = "Cannot read file (missing, empty or unreadable)";
        return std::nullopt;
    }
    return read(data);
}

std::optional<Image> PNGReader::read(std::span<const uint8_t> data) {
    Image img;
    try {
        if (!parse_datastream(data, img)) return std::nullopt;
    } catch (const std::bad_alloc&) {
        error_ = "Out of memory while decoding";
        return std::nullopt;
    } catch (const std::exception& e) {
        error_ = std::string("Decode error: ") + e.what();
        return std::nullopt;
    }
    return img;
}

bool PNGReader::parse_datastream(std::span<const uint8_t> data, Image& img) {
    warnings_.clear();
    if (data.size() < 8 + 12) {
        error_ = "File too small to be PNG";
        return false;
    }

    if (std::memcmp(data.data(), PNG_SIGNATURE, 8) != 0) {
        error_ = "Invalid PNG signature";
        return false;
    }

    size_t offset = 8;
    std::vector<uint8_t> compressed_data;
    bool seen_ihdr = false;
    bool seen_plte = false;
    bool seen_idat = false;
    bool idat_ended = false;   // a non-IDAT chunk followed the IDAT run
    bool seen_iend = false;
    bool is_apng = false;
    uint32_t expected_frames = 0;

    // APNG frame collection
    bool frame_open = false;
    FrameInfo current_frame;
    std::vector<uint8_t> current_frame_data;
    int64_t last_seq = -1;

    auto finish_frame = [&]() {
        if (!frame_open) return;
        if (current_frame.from_idat || !current_frame_data.empty()) {
            current_frame.image_data = std::move(current_frame_data);
            img.frames.push_back(std::move(current_frame));
        }
        current_frame_data.clear();
        current_frame = FrameInfo{};
        frame_open = false;
    };

    while (offset + 12 <= data.size()) {
        uint32_t length = read_big32(data.data() + offset);
        if (length > 0x7fffffffu) {
            error_ = "Chunk length exceeds 2^31-1";
            return false;
        }

        ChunkInfo chunk;
        chunk.length = length;
        std::memcpy(chunk.type, data.data() + offset + 4, 4);
        FourCC type = {chunk.type[0], chunk.type[1], chunk.type[2], chunk.type[3]};
        bool critical = is_critical(type);

        for (char c : chunk.type) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
                error_ = "Invalid chunk type";
                return false;
            }
        }

        if (static_cast<uint64_t>(offset) + 12 + length > data.size()) {
            // Truncated file. If we already have the image data, accept it
            // (like most viewers); otherwise it is unrecoverable.
            if (seen_idat && type != chunk_type::IDAT && type != chunk_type::fdAT) {
                warnings_.push_back("truncated file: missing data after " + fourcc_str(type));
                seen_iend = true;
                finish_frame();
                break;
            }
            error_ = "Chunk data exceeds file size (truncated file)";
            return false;
        }

        chunk.data = data.data() + offset + 8;
        chunk.crc = read_big32(data.data() + offset + 8 + length);
        offset += 12 + static_cast<size_t>(length);

        if (!verify_crc(chunk)) {
            if (critical) {
                error_ = "CRC mismatch in critical chunk " + fourcc_str(type);
                return false;
            }
            // Ancillary chunk damaged: drop it, keep the image (libpng default).
            warnings_.push_back("dropped " + fourcc_str(type) + " chunk with bad CRC");
            continue;
        }

        if (type == chunk_type::IHDR) {
            if (seen_ihdr) {
                error_ = "Duplicate IHDR chunk";
                return false;
            }
            if (length != 13) {
                error_ = "IHDR must be 13 bytes";
                return false;
            }
            seen_ihdr = true;

            img.width = read_big32(chunk.data);
            img.height = read_big32(chunk.data + 4);
            img.bit_depth = chunk.data[8];
            img.color_type = chunk.data[9];
            uint8_t comp = chunk.data[10], filt = chunk.data[11], ilace = chunk.data[12];
            img.interlaced = (ilace == 1);

            if (comp != 0 || filt != 0 || ilace > 1 ||
                img.width > 0x7fffffffu || img.height > 0x7fffffffu ||
                !IHDRData{img.width, img.height, img.bit_depth,
                          static_cast<ColorType>(img.color_type),
                          CompressionMethod::Deflate,
                          FilterMethod::Adaptive,
                          img.interlaced ? InterlaceMethod::Adam7 : InterlaceMethod::None}.valid()) {
                error_ = "Invalid IHDR parameters";
                return false;
            }
            uint64_t raw = static_cast<uint64_t>(img.raw_scanline_size()) * img.height;
            if (raw > MAX_RAW_IMAGE_BYTES) {
                error_ = "Image too large (" + std::to_string(img.width) + "x" +
                         std::to_string(img.height) + ")";
                return false;
            }
            continue;
        }

        if (!seen_ihdr) {
            error_ = "First chunk is not IHDR";
            return false;
        }

        if (seen_idat && type != chunk_type::IDAT) idat_ended = true;

        // ---- APNG ----
        if (type == chunk_type::acTL) {
            if (length != 8 || seen_idat) {
                warnings_.push_back("ignored misplaced/invalid acTL");
                continue;
            }
            is_apng = true;
            img.is_animated = true;
            expected_frames = read_big32(chunk.data);
            img.num_plays = read_big32(chunk.data + 4);
            continue;
        }

        if (type == chunk_type::fcTL) {
            if (!is_apng) continue;  // stray APNG chunk in a plain PNG: ignore
            if (length != 26) {
                error_ = "Invalid fcTL chunk";
                return false;
            }
            finish_frame();
            uint32_t seq = read_big32(chunk.data);
            if (static_cast<int64_t>(seq) <= last_seq) {
                error_ = "APNG sequence numbers out of order";
                return false;
            }
            last_seq = seq;
            frame_open = true;
            current_frame = FrameInfo{};
            current_frame.sequence_number = seq;
            current_frame.width = read_big32(chunk.data + 4);
            current_frame.height = read_big32(chunk.data + 8);
            current_frame.x_offset = read_big32(chunk.data + 12);
            current_frame.y_offset = read_big32(chunk.data + 16);
            current_frame.delay_num = read_big16(chunk.data + 20);
            current_frame.delay_den = read_big16(chunk.data + 22);
            current_frame.dispose_op = chunk.data[24];
            current_frame.blend_op = chunk.data[25];

            if (current_frame.width == 0 || current_frame.height == 0 ||
                static_cast<uint64_t>(current_frame.x_offset) + current_frame.width > img.width ||
                static_cast<uint64_t>(current_frame.y_offset) + current_frame.height > img.height ||
                current_frame.dispose_op > 2 || current_frame.blend_op > 1) {
                error_ = "Invalid fcTL frame region";
                return false;
            }
            // An fcTL before the first IDAT makes the IDAT image frame 0.
            if (!seen_idat) {
                if (current_frame.width != img.width || current_frame.height != img.height ||
                    current_frame.x_offset != 0 || current_frame.y_offset != 0) {
                    error_ = "First APNG frame must cover the whole image";
                    return false;
                }
                current_frame.from_idat = true;
            }
            continue;
        }

        if (type == chunk_type::fdAT) {
            if (!is_apng) continue;
            if (!frame_open || current_frame.from_idat || length < 4) {
                error_ = "fdAT without a matching fcTL";
                return false;
            }
            uint32_t seq = read_big32(chunk.data);
            if (static_cast<int64_t>(seq) <= last_seq) {
                error_ = "APNG sequence numbers out of order";
                return false;
            }
            last_seq = seq;
            current_frame_data.insert(current_frame_data.end(),
                                      chunk.data + 4, chunk.data + length);
            continue;
        }

        // ---- Critical chunks ----
        if (type == chunk_type::PLTE) {
            if (seen_plte || seen_idat) {
                error_ = "Duplicate or misplaced PLTE chunk";
                return false;
            }
            if (length == 0 || length % 3 != 0 || length > 768) {
                if (img.color_type == 3) {
                    error_ = "Invalid PLTE length";
                    return false;
                }
                warnings_.push_back("ignored invalid suggested PLTE");
                continue;
            }
            if (img.color_type == 0 || img.color_type == 4) {
                warnings_.push_back("ignored PLTE in grayscale image");
                continue;
            }
            seen_plte = true;
            // For truecolour images PLTE is only a quantization hint; it is
            // dropped so later colour-type reductions can't produce an
            // invalid file (PLTE is forbidden for grayscale).
            if (img.color_type == 3)
                img.palette.assign(chunk.data, chunk.data + length);
            continue;
        }

        if (type == chunk_type::IDAT) {
            if (img.color_type == 3 && !seen_plte) {
                error_ = "Missing PLTE chunk for indexed-color image";
                return false;
            }
            if (idat_ended) {
                // Non-consecutive IDATs violate the spec but are seen in the
                // wild; decoders concatenate them, so do the same.
                warnings_.push_back("non-consecutive IDAT chunks");
            }
            seen_idat = true;
            compressed_data.insert(compressed_data.end(), chunk.data, chunk.data + length);
            continue;
        }

        if (type == chunk_type::IEND) {
            seen_iend = true;
            finish_frame();
            break; // anything after IEND is ignored
        }

        if (critical) {
            error_ = "Unsupported critical chunk " + fourcc_str(type);
            return false;
        }

        // ---- Ancillary chunks ----
        // Fixed-size chunks with a wrong length are corrupt: drop them rather
        // than copying an invalid chunk into the output.
        auto keep = [&](std::optional<std::vector<uint8_t>>& slot, size_t min_len, size_t max_len) {
            if (seen_idat) return;  // colour info after IDAT is invalid: ignore
            if (length < min_len || length > max_len) {
                warnings_.push_back("dropped " + fourcc_str(type) + " chunk with invalid length");
                return;
            }
            slot = std::vector<uint8_t>(chunk.data, chunk.data + length);
        };
        if (type == chunk_type::tRNS) {
            if (seen_idat) continue;
            if (img.color_type == 3) {
                size_t n = std::min<size_t>(length, img.palette.size() / 3);
                if (n < length) warnings_.push_back("truncated over-long tRNS");
                img.alpha_palette.assign(chunk.data, chunk.data + n);
            } else if ((img.color_type == 0 && length == 2) ||
                       (img.color_type == 2 && length == 6)) {
                img.ancillary.tRNS.assign(chunk.data, chunk.data + length);
            } else {
                warnings_.push_back("dropped invalid tRNS");
            }
        } else if (type == chunk_type::gAMA) keep(img.ancillary.gAMA, 4, 4);
        else if (type == chunk_type::cHRM) keep(img.ancillary.cHRM, 32, 32);
        else if (type == chunk_type::sRGB) {
            if (length == 1 && chunk.data[0] > 3) warnings_.push_back("dropped sRGB with invalid intent");
            else keep(img.ancillary.sRGB, 1, 1);
        }
        else if (type == chunk_type::iCCP) {
            // name (1-79 bytes) + NUL + compression method 0 + zlib profile
            const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(chunk.data, 0, std::min<size_t>(length, 80)));
            size_t nlen = nul ? static_cast<size_t>(nul - chunk.data) : 0;
            if (!nul || nlen == 0 || nlen + 2 >= length || chunk.data[nlen + 1] != 0)
                warnings_.push_back("dropped malformed iCCP");
            else keep(img.ancillary.iCCP, 4, length);
        }
        else if (type == chunk_type::sBIT) keep(img.ancillary.sBIT, 1, 4);
        else if (type == chunk_type::bKGD) keep(img.ancillary.bKGD, 1, 6);
        else if (type == chunk_type::pHYs) keep(img.ancillary.pHYs, 9, 9);
        else if (type == chunk_type::tIME) {
            if (length == 7) img.ancillary.tIME = std::vector<uint8_t>(chunk.data, chunk.data + length);
            else warnings_.push_back("dropped tIME with invalid length");
        }
        else if (type == chunk_type::cICP) keep(img.ancillary.cICP, 4, 4);
        else if ((type == chunk_type::tEXt || type == chunk_type::zTXt || type == chunk_type::iTXt) &&
                 (length < 2 || !std::memchr(chunk.data, 0, std::min<size_t>(length, 80)) || chunk.data[0] == 0)) {
            warnings_.push_back("dropped " + fourcc_str(type) + " without a valid keyword");
        }
        else if (type == chunk_type::tEXt || type == chunk_type::zTXt || type == chunk_type::iTXt) {
            img.ancillary.text_chunks.emplace_back(
                fourcc_str(type),
                std::vector<uint8_t>(chunk.data, chunk.data + length));
        } else if (type == chunk_type::hIST || type == chunk_type::sPLT) {
            // Palette-dependent, not safe to copy after re-encoding: drop.
        } else if (type == chunk_type::eXIf || is_safe_to_copy(type)) {
            Image::Ancillary::RawChunk rc;
            rc.type = fourcc_str(type);
            rc.data.assign(chunk.data, chunk.data + length);
            rc.where = seen_idat ? 2 : (seen_plte ? 1 : 0);
            img.ancillary.other_chunks.push_back(std::move(rc));
        }
        // Unknown unsafe-to-copy ancillary chunks must be dropped when the
        // image data is modified (PNG spec 14.2).
    }

    if (!seen_ihdr) {
        error_ = "Missing IHDR chunk";
        return false;
    }
    if (!seen_idat) {
        error_ = "Missing IDAT chunk";
        return false;
    }
    if (!seen_iend) {
        // Missing IEND (truncated download) — the image data is usually
        // complete; decoding below fails if it is not.
        warnings_.push_back("missing IEND chunk");
        finish_frame();
    }

    if (!decompress_image(compressed_data, img)) return false;

    if (img.color_type == 3) normalize_palette_indices(img);

    // Decompress APNG frames. Frame 0 may simply be the IDAT image.
    if (is_apng) {
        if (img.frames.empty()) {
            // acTL without frames: treat as a plain PNG.
            img.is_animated = false;
        } else {
            if (expected_frames != img.frames.size())
                warnings_.push_back("acTL frame count does not match frames present");
            for (size_t i = 0; i < img.frames.size(); ++i) {
                if (img.frames[i].from_idat) continue;
                auto comp = std::move(img.frames[i].image_data);
                if (!decompress_frame(comp, img.frames[i], img, i))
                    return false;
            }
        }
    }

    return true;
}

void PNGReader::normalize_palette_indices(Image& img) {
    // Indices >= palette size are a spec violation that decoders handle
    // inconsistently. Make the file well-formed by padding the palette with
    // opaque black (what libpng and browsers display for such pixels).
    size_t entries = img.palette.size() / 3;
    unsigned bd = img.bit_depth;
    unsigned max_idx = 0;
    size_t row = img.raw_scanline_size();
    unsigned per = 8 / bd, mask = (1u << bd) - 1;
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* r = img.pixels.data() + y * row;
        for (size_t x = 0; x < img.width; ++x) {
            unsigned v = (r[x / per] >> (8 - bd * (x % per + 1))) & mask;
            if (v > max_idx) max_idx = v;
        }
    }
    if (max_idx >= entries) {
        warnings_.push_back("palette index out of range; padded palette");
        img.palette.resize((max_idx + 1) * 3, 0);
    }
}

bool PNGReader::unfilter_rows(const uint8_t* src, size_t src_size, size_t& offset,
                              uint8_t* dst, size_t dst_stride, uint32_t rows,
                              size_t row_bytes, size_t bpp) {
    for (uint32_t y = 0; y < rows; ++y) {
        if (offset + 1 + row_bytes > src_size) {
            error_ = "Image data truncated at row " + std::to_string(y);
            return false;
        }
        uint8_t ft = src[offset++];
        if (ft > 4) {
            error_ = "Invalid filter type " + std::to_string(ft) + " at row " + std::to_string(y);
            return false;
        }
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        const uint8_t* prev = (y > 0) ? out - dst_stride : nullptr;
        unfilter_scanline(static_cast<FilterType>(ft), src + offset, out, bpp, row_bytes, prev);
        offset += row_bytes;
    }
    return true;
}

bool PNGReader::decompress_image(const std::vector<uint8_t>& compressed_data, Image& img) {
    size_t bpp = img.bytes_per_pixel();
    size_t expected = 0;
    Adam7Pass passes[7];
    if (img.interlaced) {
        compute_adam7(img.width, img.height, passes);
        for (auto& p : passes)
            if (p.width && p.height)
                expected += (img.scanline_size_for_width(p.width) + 1) * p.height;
    } else {
        expected = (img.raw_scanline_size() + 1) * img.height;
    }

    std::vector<uint8_t> decompressed;
    try {
        // Allow a little slack: some encoders emit trailing junk inside the
        // stream; anything far beyond the expected size is a zip bomb.
        decompressed = inflate_zlib(compressed_data, expected + 65536);
    } catch (const std::exception& e) {
        error_ = std::string("Decompression error: ") + e.what();
        return false;
    }
    if (decompressed.size() < expected) {
        error_ = "Image data truncated (" + std::to_string(decompressed.size()) + " of " +
                 std::to_string(expected) + " bytes)";
        return false;
    }

    img.pixels.assign(img.total_raw_size(), 0);
    size_t offset = 0;

    if (!img.interlaced) {
        size_t row = img.raw_scanline_size();
        return unfilter_rows(decompressed.data(), decompressed.size(), offset,
                             img.pixels.data(), row, img.height, row, bpp);
    }

    size_t img_row = img.raw_scanline_size();
    for (int pass = 0; pass < 7; ++pass) {
        const auto& p = passes[pass];
        if (p.width == 0 || p.height == 0) continue;
        size_t pass_stride = img.scanline_size_for_width(p.width);
        std::vector<uint8_t> sub(pass_stride * p.height);
        if (!unfilter_rows(decompressed.data(), decompressed.size(), offset,
                           sub.data(), pass_stride, p.height, pass_stride, bpp))
            return false;
        for (uint32_t y = 0; y < p.height; ++y) {
            const uint8_t* recon = sub.data() + static_cast<size_t>(y) * pass_stride;
            size_t row_offset = static_cast<size_t>(p.y0 + y * p.dy) * img_row;
            for (uint32_t x = 0; x < p.width; ++x) {
                uint32_t img_x = p.x0 + x * p.dx;
                if (img.bit_depth < 8) {
                    size_t src_bit = static_cast<size_t>(x) * img.bit_depth;
                    size_t dst_bit = static_cast<size_t>(img_x) * img.bit_depth;
                    uint8_t val = (recon[src_bit / 8] >> (8 - img.bit_depth - src_bit % 8)) &
                                  ((1u << img.bit_depth) - 1);
                    img.pixels[row_offset + dst_bit / 8] |=
                        static_cast<uint8_t>(val << (8 - img.bit_depth - dst_bit % 8));
                } else {
                    std::memcpy(&img.pixels[row_offset + static_cast<size_t>(img_x) * bpp],
                                recon + static_cast<size_t>(x) * bpp, bpp);
                }
            }
        }
    }
    return true;
}

bool PNGReader::decompress_frame(const std::vector<uint8_t>& compressed_data,
                                  const FrameInfo& finfo, Image& img, size_t frame_idx) {
    size_t bpp = img.bytes_per_pixel();
    size_t row_size = img.scanline_size_for_width(finfo.width);
    size_t expected = (row_size + 1) * finfo.height;

    std::vector<uint8_t> decompressed;
    try {
        decompressed = inflate_zlib(compressed_data, expected + 65536);
    } catch (const std::exception& e) {
        error_ = "Frame " + std::to_string(frame_idx) + " decompression error: " + e.what();
        return false;
    }

    // APNG frames are never interlaced.
    FrameInfo& fout = img.frames[frame_idx];
    fout.image_data.assign(static_cast<size_t>(finfo.height) * row_size, 0);
    size_t offset = 0;
    if (!unfilter_rows(decompressed.data(), decompressed.size(), offset,
                       fout.image_data.data(), row_size, finfo.height, row_size, bpp)) {
        error_ = "Frame " + std::to_string(frame_idx) + ": " + error_;
        return false;
    }
    return true;
}

} // namespace fpng

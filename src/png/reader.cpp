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

// Adam7 pass dimensions
struct Adam7Pass {
    uint32_t x0, y0, dx, dy;
    uint32_t width, height;
};

static Adam7Pass adam7_passes[7] = {
    {0,0,8,8, 0,0},
    {4,0,8,8, 0,0},
    {0,4,4,8, 0,0},
    {2,0,4,4, 0,0},
    {0,2,2,4, 0,0},
    {1,0,2,2, 0,0},
    {0,1,1,2, 0,0},
};

static void compute_adam7(uint32_t w, uint32_t h) {
    adam7_passes[0].width = (w + 7) / 8;
    adam7_passes[0].height = (h + 7) / 8;
    adam7_passes[1].width = (w + 3) / 8;
    adam7_passes[1].height = (h + 7) / 8;
    adam7_passes[2].width = (w + 3) / 4;
    adam7_passes[2].height = (h + 3) / 8;
    adam7_passes[3].width = (w + 1) / 4;
    adam7_passes[3].height = (h + 3) / 4;
    adam7_passes[4].width = (w + 1) / 2;
    adam7_passes[4].height = (h + 1) / 4;
    adam7_passes[5].width = w / 2;
    adam7_passes[5].height = (h + 1) / 2;
    adam7_passes[6].width = w;
    adam7_passes[6].height = h / 2;
}

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
        error_ = "Cannot read file: " + path.string();
        return std::nullopt;
    }
    return read(data);
}

std::optional<Image> PNGReader::read(std::span<const uint8_t> data) {
    Image img;
    if (!parse_datastream(data, img)) return std::nullopt;
    return img;
}

bool PNGReader::parse_datastream(std::span<const uint8_t> data, Image& img) {
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
    bool seen_idat = false;
    bool seen_iend = false;
    bool is_apng = false;

    // Temporary storage for APNG
    bool in_apng_sequence = false;
    FrameInfo current_frame;
    std::vector<uint8_t> current_frame_data;

    while (offset + 12 <= data.size()) {
        uint32_t length = read_big32(data.data() + offset);
        offset += 4;

        ChunkInfo chunk;
        chunk.length = length;
        std::memcpy(chunk.type, data.data() + offset, 4);
        offset += 4;

        if (offset + length + 4 > data.size()) {
            error_ = "Chunk data exceeds file size";
            return false;
        }

        chunk.data = data.data() + offset;
        offset += length;

        chunk.crc = read_big32(data.data() + offset);
        offset += 4;

        if (!verify_crc(chunk)) {
            error_ = "CRC mismatch in chunk";
            return false;
        }

        FourCC type = {chunk.type[0], chunk.type[1], chunk.type[2], chunk.type[3]};

        if (seen_iend) {
            error_ = "Data after IEND chunk";
            return false;
        }

        // IHDR must be first
        if (type == chunk_type::IHDR) {
            if (seen_ihdr || seen_idat) {
                error_ = "IHDR must be first chunk";
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
            img.interlaced = (chunk.data[12] == 1);

            if (!IHDRData{img.width, img.height, img.bit_depth,
                          static_cast<ColorType>(img.color_type),
                          CompressionMethod::Deflate,
                          FilterMethod::Adaptive,
                          img.interlaced ? InterlaceMethod::Adam7 : InterlaceMethod::None}.valid()) {
                error_ = "Invalid IHDR parameters";
                return false;
            }
            continue;
        }

        if (!seen_ihdr) {
            error_ = "Chunks before IHDR";
            return false;
        }

        // APNG chunks
        if (type == chunk_type::acTL) {
            if (length != 8) {
                error_ = "acTL must be 8 bytes";
                return false;
            }
            is_apng = true;
            img.is_animated = true;
            [[maybe_unused]] uint32_t expected_frames = read_big32(chunk.data);
            img.num_plays = read_big32(chunk.data + 4);
            continue;
        }

        if (type == chunk_type::fcTL) {
            if (!is_apng || length != 26) {
                error_ = "Invalid fcTL chunk";
                return false;
            }
            // If we had a previous frame being collected, finalize it
            if (in_apng_sequence && !current_frame_data.empty()) {
                current_frame.image_data = std::move(current_frame_data);
                img.frames.push_back(std::move(current_frame));
                current_frame_data.clear();
            }

            in_apng_sequence = true;
            current_frame = FrameInfo{};
            current_frame.sequence_number = read_big32(chunk.data);
            current_frame.width = read_big32(chunk.data + 4);
            current_frame.height = read_big32(chunk.data + 8);
            current_frame.x_offset = read_big32(chunk.data + 12);
            current_frame.y_offset = read_big32(chunk.data + 16);
            current_frame.delay_num = read_big16(chunk.data + 20);
            current_frame.delay_den = read_big16(chunk.data + 22);
            current_frame.dispose_op = chunk.data[24];
            current_frame.blend_op = chunk.data[25];

            if (current_frame.width == 0 || current_frame.height == 0) {
                current_frame.width = img.width;
                current_frame.height = img.height;
            }
            continue;
        }

        if (type == chunk_type::fdAT) {
            if (!is_apng || !in_apng_sequence) {
                error_ = "fdAT without active frame";
                return false;
            }
            if (length < 4) {
                error_ = "fdAT too small";
                return false;
            }
            uint32_t seq = read_big32(chunk.data);
            if (seq != current_frame.sequence_number) {
                error_ = "fdAT sequence number mismatch";
                return false;
            }
            current_frame_data.insert(current_frame_data.end(),
                                       chunk.data + 4, chunk.data + length);
            continue;
        }

        // Regular chunks
        if (type == chunk_type::PLTE) {
            if (img.color_type != 3 && img.color_type != 2 && img.color_type != 6) {
                error_ = "PLTE only valid for color types 2,3,6";
                return false;
            }
            if (length % 3 != 0 || length > 768) {
                error_ = "PLTE length must be multiple of 3, max 768";
                return false;
            }
            img.palette.assign(chunk.data, chunk.data + length);
            continue;
        }

        if (type == chunk_type::tRNS) {
            if (img.color_type == 3) {
                img.alpha_palette.assign(chunk.data, chunk.data + length);
            } else {
                img.ancillary.tRNS.assign(chunk.data, chunk.data + length);
            }
            continue;
        }

        if (type == chunk_type::IDAT) {
            if (seen_idat && !is_apng) {
                // OK: multiple IDATs allowed
            }
            if (!seen_idat) {
                seen_idat = true;
            }
            compressed_data.insert(compressed_data.end(), chunk.data, chunk.data + length);
            continue;
        }

        if (type == chunk_type::IEND) {
            seen_iend = true;

            // Finalize last APNG frame
            if (in_apng_sequence && !current_frame_data.empty()) {
                current_frame.image_data = std::move(current_frame_data);
                img.frames.push_back(std::move(current_frame));
            }

            break;
        }

        // Preserve ancillary chunks
        if (type == chunk_type::gAMA)
            img.ancillary.gAMA = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::cHRM)
            img.ancillary.cHRM = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::sRGB)
            img.ancillary.sRGB = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::iCCP)
            img.ancillary.iCCP = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::sBIT)
            img.ancillary.sBIT = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::bKGD)
            img.ancillary.bKGD = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::pHYs)
            img.ancillary.pHYs = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::tIME)
            img.ancillary.tIME = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::cICP)
            img.ancillary.cICP = std::vector<uint8_t>(chunk.data, chunk.data + length);
        else if (type == chunk_type::tEXt || type == chunk_type::zTXt || type == chunk_type::iTXt) {
            std::string fcc(reinterpret_cast<const char*>(type.data()), 4);
            img.ancillary.text_chunks.emplace_back(
                fcc,
                std::vector<uint8_t>(chunk.data, chunk.data + length));
        }
    }

    if (!seen_iend) {
        error_ = "Missing IEND chunk";
        return false;
    }

    // Decompress the static image
    if (!compressed_data.empty()) {
        if (!decompress_image(compressed_data, img)) return false;
    }

    // Decompress APNG frame data
    for (size_t i = 0; i < img.frames.size(); ++i) {
        if (!img.frames[i].image_data.empty()) {
            if (!decompress_frame(img.frames[i].image_data, img.frames[i], img, i))
                return false;
        }
    }

    return true;
}

bool PNGReader::decompress_image(const std::vector<uint8_t>& compressed_data, Image& img) {
    std::vector<uint8_t> decompressed;
    try {
        decompressed = inflate_zlib(compressed_data);
    } catch (const std::exception& e) {
        error_ = std::string("Decompression error: ") + e.what();
        return false;
    }

    size_t bpp = img.bytes_per_pixel();
    size_t stride = bpp;

    if (img.interlaced) {
        compute_adam7(img.width, img.height);

        img.pixels.resize(img.total_raw_size());
        size_t offset = 0;

        for (int pass = 0; pass < 7; ++pass) {
            auto& p = adam7_passes[pass];
            if (p.width == 0 || p.height == 0) continue;

            size_t pass_stride = (img.color_type == 3)
                ? ((static_cast<size_t>(p.width) * img.bit_depth + 7) / 8)
                : (static_cast<size_t>(p.width) * bpp);

            std::vector<uint8_t> prev_scanline(pass_stride, 0);

            for (uint32_t y = 0; y < p.height; ++y) {
                if (offset >= decompressed.size()) {
                    error_ = "Decompressed data too small for Adam7 pass " + std::to_string(pass);
                    return false;
                }

                uint8_t filter_byte = decompressed[offset++];
                FilterType ft = static_cast<FilterType>(filter_byte);

                if (offset + pass_stride > decompressed.size()) {
                    error_ = "Decompressed data truncated in Adam7 pass " + std::to_string(pass);
                    return false;
                }

                std::vector<uint8_t> recon(pass_stride);
                unfilter_scanline(ft, decompressed.data() + offset,
                                  recon.data(), stride, pass_stride,
                                  y > 0 ? prev_scanline.data() : nullptr);

                offset += pass_stride;

                // Scatter into full image
                uint32_t img_y = p.y0 + y * p.dy;
                if (img_y < img.height) {
                    size_t row_offset = static_cast<size_t>(img_y) * img.raw_scanline_size();
                    for (uint32_t x = 0; x < p.width; ++x) {
                        uint32_t img_x = p.x0 + x * p.dx;
                        if (img_x >= img.width) continue;

                        if (img.color_type == 3) {
                            // Bit-packed indexed
                            size_t src_byte = static_cast<size_t>(x) * img.bit_depth / 8;
                            size_t dst_byte = static_cast<size_t>(img_x) * img.bit_depth / 8;
                            int src_bit = static_cast<int>((x * img.bit_depth) % 8);
                            int dst_bit = static_cast<int>((img_x * img.bit_depth) % 8);
                            uint8_t val = (recon[src_byte] >> (8 - img.bit_depth - src_bit)) &
                                          ((1u << img.bit_depth) - 1);
                            img.pixels[row_offset + dst_byte] |= static_cast<uint8_t>(
                                val << (8 - img.bit_depth - dst_bit));
                        } else {
                            size_t src_off = static_cast<size_t>(x) * bpp;
                            size_t dst_off = static_cast<size_t>(img_x) * bpp;
                            for (size_t b = 0; b < bpp; ++b)
                                img.pixels[row_offset + dst_off + b] = recon[src_off + b];
                        }
                    }
                }

                std::swap(prev_scanline, recon);
            }
        }
    } else {
        img.pixels.resize(img.total_raw_size());
        size_t offset = 0;

        for (uint32_t y = 0; y < img.height; ++y) {
            if (offset >= decompressed.size()) {
                error_ = "Decompressed data too small at row " + std::to_string(y);
                return false;
            }

            uint8_t filter_byte = decompressed[offset++];
            FilterType ft = static_cast<FilterType>(filter_byte);

            size_t row_size = img.raw_scanline_size();
            if (offset + row_size > decompressed.size()) {
                error_ = "Decompressed data truncated at row " + std::to_string(y);
                return false;
            }

            size_t row_off = static_cast<size_t>(y) * row_size;
            const uint8_t* prev = (y > 0) ? &img.pixels[row_off - row_size] : nullptr;

            unfilter_scanline(ft, decompressed.data() + offset,
                              img.pixels.data() + row_off,
                              stride, row_size, prev);

            offset += row_size;
        }
    }

    return true;
}

bool PNGReader::decompress_frame(const std::vector<uint8_t>& compressed_data,
                                  const FrameInfo& finfo, Image& img, size_t frame_idx) {
    (void)img;
    (void)frame_idx;

    std::vector<uint8_t> decompressed;
    try {
        decompressed = inflate_zlib(compressed_data);
    } catch (const std::exception& e) {
        error_ = std::string("Frame decompression error: ") + e.what();
        return false;
    }

    size_t bpp = img.bytes_per_pixel();
    size_t stride = bpp;
    size_t row_size = (img.color_type == 3)
        ? ((static_cast<size_t>(finfo.width) * img.bit_depth + 7) / 8)
        : (static_cast<size_t>(finfo.width) * bpp);

    // Note: APNG frames are never interlaced
    size_t offset = 0;
    FrameInfo& fout = img.frames[frame_idx];
    fout.image_data.clear();
    fout.image_data.resize(static_cast<size_t>(finfo.height) * row_size);

    for (uint32_t y = 0; y < finfo.height; ++y) {
        if (offset >= decompressed.size()) break;
        uint8_t filter_byte = decompressed[offset++];
        FilterType ft = static_cast<FilterType>(filter_byte);

        if (offset + row_size > decompressed.size()) {
            error_ = "Frame data truncated at row " + std::to_string(y);
            return false;
        }

        size_t row_off = static_cast<size_t>(y) * row_size;
        const uint8_t* prev = (y > 0) ? &fout.image_data[row_off - row_size] : nullptr;

        unfilter_scanline(ft, decompressed.data() + offset,
                          fout.image_data.data() + row_off,
                          stride, row_size, prev);

        offset += row_size;
    }

    return true;
}

} // namespace fpng

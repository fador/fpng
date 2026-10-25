#pragma once

#include "image/image.hpp"

#include <vector>
#include <cstdint>
#include <span>
#include <filesystem>
#include <optional>
#include <string>

namespace fpng {

struct ChunkInfo {
    uint32_t length;
    char     type[4];
    const uint8_t* data;
    uint32_t crc;
};

class PNGReader {
public:
    PNGReader() = default;

    std::optional<Image> read(const std::filesystem::path& path);
    std::optional<Image> read(std::span<const uint8_t> data);

    const std::string& error() const noexcept { return error_; }
    // Non-fatal problems that were repaired or ignored while reading.
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

private:
    bool parse_datastream(std::span<const uint8_t> data, Image& img);
    bool decompress_image(const std::vector<uint8_t>& compressed_data, Image& img);
    bool decompress_frame(const std::vector<uint8_t>& compressed_data,
                          const FrameInfo& finfo, Image& img, size_t frame_idx);
    bool verify_crc(const ChunkInfo& chunk) const;
    bool unfilter_rows(const uint8_t* src, size_t src_size, size_t& offset,
                       uint8_t* dst, size_t dst_stride, uint32_t rows,
                       size_t row_bytes, size_t bpp);
    void normalize_palette_indices(Image& img);

    std::string error_;
    std::vector<std::string> warnings_;
};

} // namespace fpng

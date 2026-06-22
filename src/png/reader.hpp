#pragma once

#include "image/image.hpp"

#include <vector>
#include <cstdint>
#include <span>
#include <filesystem>
#include <optional>

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

private:
    bool parse_datastream(std::span<const uint8_t> data, Image& img);
    bool process_chunk(const ChunkInfo& chunk, Image& img,
                       std::vector<uint8_t>& compressed_data,
                       bool& seen_idat, bool& seen_iend);
    bool decompress_image(const std::vector<uint8_t>& compressed_data, Image& img);
    bool decompress_frame(const std::vector<uint8_t>& compressed_data,
                          const FrameInfo& finfo, Image& img, size_t frame_idx);
    bool verify_crc(const ChunkInfo& chunk) const;

    std::string error_;
};

} // namespace fpng

#pragma once

#include "image/image.hpp"
#include "compress/deflate/deflater.hpp"

#include <vector>
#include <cstdint>
#include <span>
#include <filesystem>
#include <string>

namespace fpng {

enum class FilterType : uint8_t;

struct WriteOptions {
    std::vector<FilterType> filters;   // pre-computed filter choices (empty = auto)
    DeflateOptions deflate;            // deflate parameters
    // Adam7 interlacing almost always compresses worse than the same image
    // written progressively off; default to non-interlaced (the in-memory
    // pixels are already de-interlaced by the reader).
    bool interlace = false;
};

class PNGWriter {
public:
    PNGWriter() = default;

    bool write(const std::filesystem::path& path, const Image& img,
               const WriteOptions& wopts = {});
    std::vector<uint8_t> write(const Image& img,
                                const WriteOptions& wopts = {});

    const std::string& error() const noexcept { return error_; }

private:
    void write_signature(std::vector<uint8_t>& out);
    void write_ihdr(std::vector<uint8_t>& out, const Image& img,
                    const WriteOptions& wopts);
    void write_ancillary(std::vector<uint8_t>& out, const Image& img);
    void write_plte(std::vector<uint8_t>& out, const Image& img);
    void write_trns(std::vector<uint8_t>& out, const Image& img);
    void write_apng_chunks(std::vector<uint8_t>& out, const Image& img);
    void write_idat(std::vector<uint8_t>& out, const Image& img,
                     const WriteOptions& wopts);
    void write_iend(std::vector<uint8_t>& out);

    void write_chunk(std::vector<uint8_t>& out,
                     const char type[4],
                     std::span<const uint8_t> data);

    std::vector<uint8_t> filter_and_compress(const Image& img,
                                              const WriteOptions& wopts);
    std::vector<uint8_t> filter_and_compress_frame(const Image& img,
                                                     const FrameInfo& finfo);

    std::string error_;
};

} // namespace fpng

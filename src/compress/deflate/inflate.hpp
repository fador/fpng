#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <cstdint>

namespace fpng {

// Decompress a zlib / raw DEFLATE stream. Throws std::runtime_error on any
// malformed input. `max_output` bounds the decompressed size (decompression-
// bomb guard); exceeding it is an error.
std::vector<uint8_t> inflate_zlib(std::span<const uint8_t> data,
                                  size_t max_output = SIZE_MAX);
std::vector<uint8_t> inflate_raw(std::span<const uint8_t> data,
                                 size_t max_output = SIZE_MAX);

} // namespace fpng

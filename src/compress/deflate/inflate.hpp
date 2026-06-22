#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>

namespace fpng {

std::vector<uint8_t> inflate_zlib(std::span<const uint8_t> data);
std::vector<uint8_t> inflate_raw(std::span<const uint8_t> data);

} // namespace fpng

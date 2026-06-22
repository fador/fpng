#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

namespace fpng {

class CRC32 {
public:
    CRC32() noexcept : crc_(0xffffffffu) {}

    CRC32& update(const uint8_t* data, size_t len) noexcept;
    CRC32& update(std::span<const uint8_t> data) noexcept;

    uint32_t finalize() const noexcept { return crc_ ^ 0xffffffffu; }
    uint32_t value() const noexcept { return crc_; }

    static uint32_t compute(const uint8_t* data, size_t len) noexcept {
        CRC32 c;
        c.update(data, len);
        return c.finalize();
    }

    static uint32_t compute(std::span<const uint8_t> data) noexcept {
        return compute(data.data(), data.size());
    }

    void reset() noexcept { crc_ = 0xffffffffu; }

private:
    uint32_t crc_;
    static const uint32_t table_[256];
};

} // namespace fpng

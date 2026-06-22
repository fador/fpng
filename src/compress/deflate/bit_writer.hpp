#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>

namespace fpng {

class BitWriter {
public:
    BitWriter() { reset(); }

    void write_bits(uint32_t value, int num_bits);
    void write_byte(uint8_t value);
    void write_bytes(std::span<const uint8_t> data);
    void flush_to_byte();

    void reset();

    int bit_count() const noexcept { return bits_in_buf_; }
    int byte_count() const noexcept { return byte_pos_; }
    const std::vector<uint8_t>& bytes() const noexcept { return buf_; }

    void align_to_byte();

private:
    std::vector<uint8_t> buf_;
    int byte_pos_ = 0;
    uint64_t bit_buf_ = 0;
    int bits_in_buf_ = 0;

    void ensure_capacity(int extra_bytes);
};

inline void BitWriter::write_bits(uint32_t value, int num_bits) {
    bit_buf_ |= static_cast<uint64_t>(value) << bits_in_buf_;
    bits_in_buf_ += num_bits;

    while (bits_in_buf_ >= 8) {
        ensure_capacity(1);
        buf_[byte_pos_++] = static_cast<uint8_t>(bit_buf_ & 0xff);
        bit_buf_ >>= 8;
        bits_in_buf_ -= 8;
    }
}

inline void BitWriter::write_byte(uint8_t value) {
    write_bits(value, 8);
}

inline void BitWriter::write_bytes(std::span<const uint8_t> data) {
    for (auto b : data) write_byte(b);
}

inline void BitWriter::flush_to_byte() {
    if (bits_in_buf_ > 0) {
        ensure_capacity(1);
        buf_[byte_pos_++] = static_cast<uint8_t>(bit_buf_ & 0xff);
        bit_buf_ = 0;
        bits_in_buf_ = 0;
    }
}

inline void BitWriter::align_to_byte() {
    int skip = bits_in_buf_ & 7;
    if (skip) {
        bit_buf_ >>= skip;
        bits_in_buf_ -= skip;
    }
}

inline void BitWriter::reset() {
    buf_.resize(4096);
    byte_pos_ = 0;
    bit_buf_ = 0;
    bits_in_buf_ = 0;
}

inline void BitWriter::ensure_capacity(int extra_bytes) {
    while (byte_pos_ + extra_bytes >= static_cast<int>(buf_.size()))
        buf_.resize(buf_.size() * 2);
}

} // namespace fpng

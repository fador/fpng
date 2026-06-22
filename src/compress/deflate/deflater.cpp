#include "compress/deflate/deflater.hpp"
#include "compress/deflate/bit_writer.hpp"

#include <cstring>
#include <stdexcept>

namespace fpng {

// Minimal DEFLATE compressor using stored blocks only (zero compression).
// The full compressor with optimal LZ77/Huffman/block splitting will replace this.
std::vector<uint8_t> deflate_compress(std::span<const uint8_t> data,
                                       const DeflateOptions& opts) {
    (void)opts;

    std::vector<uint8_t> output;
    output.reserve(data.size() + data.size() / 65535 * 5 + 10);

    BitWriter bw;
    size_t pos = 0;

    while (pos < data.size()) {
        size_t block_size = std::min(data.size() - pos, size_t(65535));
        bool is_last = (pos + block_size >= data.size());

        bw.write_bits(is_last ? 1 : 0, 1); // BFINAL
        bw.write_bits(0, 2);                // BTYPE = stored
        bw.flush_to_byte();

        // Store block header
        uint16_t len = static_cast<uint16_t>(block_size);
        uint16_t nlen = static_cast<uint16_t>(~len);

        output.insert(output.end(),
                       reinterpret_cast<const uint8_t*>(&bw.bytes()[0]),
                       reinterpret_cast<const uint8_t*>(&bw.bytes()[0]) + bw.byte_count());
        bw.reset();
        // No bits buffered after flush_to_byte

        output.push_back(static_cast<uint8_t>(len & 0xff));
        output.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        output.push_back(static_cast<uint8_t>(nlen & 0xff));
        output.push_back(static_cast<uint8_t>((nlen >> 8) & 0xff));

        output.insert(output.end(), data.data() + pos, data.data() + pos + block_size);
        pos += block_size;
    }

    // Flush remaining bits
    bw.flush_to_byte();
    if (bw.bit_count() > 0) {
        output.insert(output.end(),
                       reinterpret_cast<const uint8_t*>(&bw.bytes()[0]),
                       reinterpret_cast<const uint8_t*>(&bw.bytes()[0]) + bw.byte_count());
    }

    return output;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::vector<uint8_t> zlib_compress(std::span<const uint8_t> data,
                                    const DeflateOptions& opts) {
    // RFC 1950 zlib header
    uint8_t cmf = 0x78; // CM=8(deflate), CINFO=7(32K window)
    uint8_t flg = 0x01; // level hint, no dict
    // Make CMF*256+FLG divisible by 31
    uint16_t check = static_cast<uint16_t>(cmf) * 256 + flg;
    if (check % 31 != 0) {
        flg += static_cast<uint8_t>(31 - (check % 31));
    }

    auto compressed = deflate_compress(data, opts);

    std::vector<uint8_t> result;
    result.reserve(2 + compressed.size() + 4);
    result.push_back(cmf);
    result.push_back(flg);
    result.insert(result.end(), compressed.begin(), compressed.end());

    uint32_t adler = adler32(data.data(), data.size());
    result.push_back(static_cast<uint8_t>((adler >> 24) & 0xff));
    result.push_back(static_cast<uint8_t>((adler >> 16) & 0xff));
    result.push_back(static_cast<uint8_t>((adler >> 8) & 0xff));
    result.push_back(static_cast<uint8_t>(adler & 0xff));

    return result;
}

} // namespace fpng

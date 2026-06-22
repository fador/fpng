#include "compress/deflate/deflater.hpp"
#include "compress/deflate/huffman.hpp"
#include "compress/deflate/block_splitter.hpp"
#include "compress/deflate/bit_writer.hpp"

#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace fpng {

namespace {

// Forward declarations
void write_stored_block(const uint8_t* data, size_t size,
                         bool is_last, std::vector<uint8_t>& out);
void write_fixed_block(const uint8_t* data, size_t size,
                        bool is_last, const DeflateOptions& opts,
                        std::vector<uint8_t>& out);
void write_dynamic_block(const uint8_t* data, size_t size,
                          bool is_last, const DeflateOptions& opts,
                          std::vector<uint8_t>& out);

// Fixed Huffman code table (RFC 1951 section 3.2.6)
// Codes are MSB-first; we'll reverse for LSB-first output
struct FixedCode { uint32_t code; int bits; };

FixedCode get_fixed_litlen(int sym) {
    if (sym <= 143) return {static_cast<uint32_t>(sym + 0x30), 8};
    if (sym <= 255) return {static_cast<uint32_t>(sym - 144 + 0x190), 9};
    if (sym == 256) return {0, 7};
    if (sym <= 279) return {static_cast<uint32_t>(sym - 256), 7};
    /* 280-287 */  return {static_cast<uint32_t>(sym - 280 + 0xC0), 8};
}

FixedCode get_fixed_dist(int code) {
    return {static_cast<uint32_t>(code), 5};
}

// Encode using fixed Huffman blocks
void write_fixed_block(const uint8_t* data, size_t size,
                        bool is_last, const DeflateOptions& opts,
                        std::vector<uint8_t>& out) {
    LZ77Parser parser;
    LZ77Parser::Options parse_opts;
    parse_opts.optimal = opts.optimal_parsing;
    parse_opts.lazy_matching = !opts.optimal_parsing;
    auto tokens = parser.parse(data, size, parse_opts);

    BitWriter bw;
    bw.write_bits(is_last ? 1 : 0, 1);  // BFINAL
    bw.write_bits(1, 2);                 // BTYPE = fixed Huffman

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        auto& t = tokens[i];
        if (t.type == LZ77Parser::Token::LITERAL) {
            auto fc = get_fixed_litlen(t.literal);
            bw.write_bits(reverse_bits_u32(fc.code, fc.bits), fc.bits);
        } else {
            int lc = deflate::length_code(t.match_length);
            auto fl = get_fixed_litlen(257 + lc);
            bw.write_bits(reverse_bits_u32(fl.code, fl.bits), fl.bits);
            int extra = deflate::length_extra_bits(lc);
            if (extra) bw.write_bits(t.match_length - deflate::length_base(lc), extra);

            int dc = deflate::distance_code(t.match_distance);
            auto fd = get_fixed_dist(dc);
            bw.write_bits(reverse_bits_u32(fd.code, fd.bits), fd.bits);
            int dextra = deflate::distance_extra_bits(dc);
            if (dextra) bw.write_bits(t.match_distance - deflate::distance_base(dc), dextra);
        }
    }

    // EOB
    auto eob = get_fixed_litlen(256);
    bw.write_bits(reverse_bits_u32(eob.code, eob.bits), eob.bits);
    bw.flush_to_byte();

    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(bw.bytes().data()),
               reinterpret_cast<const uint8_t*>(bw.bytes().data()) + bw.byte_count());
}

void write_stored_block(const uint8_t* data, size_t size,
                         bool is_last, std::vector<uint8_t>& out) {
    BitWriter bw;
    bw.write_bits(is_last ? 1 : 0, 1);
    bw.write_bits(0, 2);
    bw.flush_to_byte();

    size_t pos = out.size();
    out.resize(pos + 4);
    uint16_t len = static_cast<uint16_t>(size);
    out[pos]     = static_cast<uint8_t>(len & 0xff);
    out[pos + 1] = static_cast<uint8_t>((len >> 8) & 0xff);
    out[pos + 2] = static_cast<uint8_t>((~len) & 0xff);
    out[pos + 3] = static_cast<uint8_t>(((~len) >> 8) & 0xff);
    out.insert(out.end(), data, data + size);
}

uint32_t compute_adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

} // anonymous namespace

std::vector<uint8_t> Deflater::compress(std::span<const uint8_t> data,
                                         const DeflateOptions& opts) {
    if (data.empty()) return {};

    if (opts.level == CompressionLevel::Store) {
        std::vector<uint8_t> out;
        size_t pos = 0;
        while (pos < data.size()) {
            size_t sz = std::min(data.size() - pos, size_t(65535));
            write_stored_block(data.data() + pos, sz,
                               pos + sz >= data.size(), out);
            pos += sz;
        }
        return out;
    }

    // Use fixed Huffman blocks for compression
    auto blocks = opts.adaptive_blocks
        ? BlockSplitter::split_adaptive(data.data(), data.size(),
                                         1024, opts.max_block_size)
        : BlockSplitter::split(data.data(), data.size(), opts.max_block_size);

    if (blocks.empty())
        blocks.push_back({0, data.size()});

    std::vector<uint8_t> out;
    out.reserve(data.size() + blocks.size() * 100);

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        auto& b = blocks[bi];
        bool is_last = (bi == blocks.size() - 1);
        if (opts.level <= CompressionLevel::Store)
            write_stored_block(data.data() + b.start_offset,
                              b.end_offset - b.start_offset,
                              is_last, out);
        else
            write_fixed_block(data.data() + b.start_offset,
                             b.end_offset - b.start_offset,
                             is_last, opts, out);
    }

    return out;
}

std::vector<uint8_t> Deflater::compress_zlib(std::span<const uint8_t> data,
                                              const DeflateOptions& opts) {
    auto deflated = compress(data, opts);

    uint8_t cmf = 0x78;
    uint8_t flg = 0x01;
    uint16_t check = (static_cast<uint16_t>(cmf) << 8) | flg;
    if (check % 31 != 0)
        flg += static_cast<uint8_t>(31 - (check % 31));

    std::vector<uint8_t> result;
    result.reserve(2 + deflated.size() + 4);
    result.push_back(cmf);
    result.push_back(flg);
    result.insert(result.end(), deflated.begin(), deflated.end());

    uint32_t adler = compute_adler32(data.data(), data.size());
    result.push_back(static_cast<uint8_t>((adler >> 24) & 0xff));
    result.push_back(static_cast<uint8_t>((adler >> 16) & 0xff));
    result.push_back(static_cast<uint8_t>((adler >> 8) & 0xff));
    result.push_back(static_cast<uint8_t>(adler & 0xff));

    return result;
}

// Convenience wrappers
std::vector<uint8_t> deflate_compress(std::span<const uint8_t> data,
                                       const DeflateOptions& opts) {
    Deflater d;
    return d.compress(data, opts);
}

std::vector<uint8_t> zlib_compress(std::span<const uint8_t> data,
                                    const DeflateOptions& opts) {
    Deflater d;
    return d.compress_zlib(data, opts);
}

} // namespace fpng

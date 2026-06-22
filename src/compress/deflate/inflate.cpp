#include "compress/deflate/inflate.hpp"

#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <vector>
#include <span>

namespace fpng {

namespace {

constexpr int MAX_BITS = 15;

constexpr int LEN_EXTRA_BITS[] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};
constexpr int LEN_BASE[] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31, 35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
constexpr int DIST_EXTRA_BITS[] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6, 7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};
constexpr int DIST_BASE[] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193, 257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289, 16385,24577
};

constexpr int CLEN_ORDER[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t byte_pos = 0;
    uint64_t bit_buf = 0;
    int bits_in_buf = 0;

    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    uint32_t read_bits(int n) {
        while (bits_in_buf < n) {
            if (byte_pos >= size) throw std::runtime_error("inflate: unexpected EOF");
            bit_buf |= static_cast<uint64_t>(data[byte_pos++]) << bits_in_buf;
            bits_in_buf += 8;
        }
        uint32_t val = static_cast<uint32_t>(bit_buf & ((1ull << n) - 1));
        bit_buf >>= n;
        bits_in_buf -= n;
        return val;
    }

    uint32_t peek_bits(int n) const {
        if (bits_in_buf < n) return 0xffffffff;
        return static_cast<uint32_t>(bit_buf & ((1ull << n) - 1));
    }

    void align_to_byte() {
        int skip = bits_in_buf & 7;
        if (skip) { bit_buf >>= skip; bits_in_buf -= skip; }
    }
};

// Canonical Huffman tree with sorted symbol lookup
struct HuffmanTree {
    struct Entry {
        uint16_t code;
        uint8_t  bits;
        uint16_t symbol;
    };

    std::vector<Entry> entries;

    void build(const int* lengths, int num_syms) {
        // Count lengths
        int bl_count[MAX_BITS + 1] = {};
        for (int i = 0; i < num_syms; ++i)
            if (lengths[i] > 0) bl_count[lengths[i]]++;

        // Generate codes
        uint16_t next_code[MAX_BITS + 1] = {};
        uint16_t code = 0;
        for (int bits = 1; bits <= MAX_BITS; ++bits) {
            code = static_cast<uint16_t>((code + bl_count[bits - 1]) << 1);
            next_code[bits] = code;
        }

        entries.clear();
        for (int sym = 0; sym < num_syms; ++sym) {
            int len = lengths[sym];
            if (len > 0) {
                entries.push_back({next_code[len], static_cast<uint8_t>(len), static_cast<uint16_t>(sym)});
                next_code[len]++;
            }
        }

        // Sort by (bits, code) for decoding
        std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) {
                if (a.bits != b.bits) return a.bits < b.bits;
                return a.code < b.code;
            });
    }

    int decode(BitReader& br) const {
        uint32_t bits = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            while (static_cast<int>(br.bits_in_buf) < e.bits) {
                if (br.byte_pos >= br.size) throw std::runtime_error("inflate: EOF in decode");
                br.bit_buf |= static_cast<uint64_t>(br.data[br.byte_pos++]) << br.bits_in_buf;
                br.bits_in_buf += 8;
            }
            bits = static_cast<uint32_t>(br.bit_buf & ((1u << e.bits) - 1));
            if (bits == e.code) {
                br.bit_buf >>= e.bits;
                br.bits_in_buf -= e.bits;
                return e.symbol;
            }
        }
        throw std::runtime_error("inflate: invalid huffman code");
    }
};

static void decompress_block(BitReader& br, std::vector<uint8_t>& output, bool& done) {
    int bfinal = br.read_bits(1);
    int btype  = br.read_bits(2);

    if (btype == 0) {
        // Stored block
        br.align_to_byte();
        if (br.byte_pos + 4 > br.size) throw std::runtime_error("inflate: eof in stored header");
        uint16_t len  = static_cast<uint16_t>(br.data[br.byte_pos]) |
                        (static_cast<uint16_t>(br.data[br.byte_pos + 1]) << 8);
        uint16_t nlen = static_cast<uint16_t>(br.data[br.byte_pos + 2]) |
                        (static_cast<uint16_t>(br.data[br.byte_pos + 3]) << 8);
        if (len != (nlen ^ 0xffff)) throw std::runtime_error("inflate: stored len mismatch");
        br.byte_pos += 4;
        br.bits_in_buf = 0; br.bit_buf = 0;
        if (br.byte_pos + len > br.size) throw std::runtime_error("inflate: stored overflow");
        output.insert(output.end(), br.data + br.byte_pos, br.data + br.byte_pos + len);
        br.byte_pos += len;
        done = bfinal;
        return;
    }

    if (btype == 1) {
        // Fixed Huffman
        int litlen_len[288], dist_len[32];
        for (int i = 0; i <= 143; ++i) litlen_len[i] = 8;
        for (int i = 144; i <= 255; ++i) litlen_len[i] = 9;
        for (int i = 256; i <= 279; ++i) litlen_len[i] = 7;
        for (int i = 280; i <= 287; ++i) litlen_len[i] = 8;
        for (int i = 0; i < 32; ++i) dist_len[i] = 5;

        HuffmanTree litlen_tree, dist_tree;
        litlen_tree.build(litlen_len, 288);
        dist_tree.build(dist_len, 32);

        for (;;) {
            int sym = litlen_tree.decode(br);
            if (sym < 256) {
                output.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break;
            } else {
                int code = sym - 257;
                int length = LEN_BASE[code] + br.read_bits(LEN_EXTRA_BITS[code]);
                int dcode = dist_tree.decode(br);
                int distance = DIST_BASE[dcode] + br.read_bits(DIST_EXTRA_BITS[dcode]);
                if (distance > static_cast<int>(output.size()))
                    throw std::runtime_error("inflate: invalid distance");
                size_t src = output.size() - distance;
                for (int i = 0; i < length; ++i)
                    output.push_back(output[src + i]);
            }
        }
        done = bfinal;
        return;
    }

    if (btype == 2) {
        // Dynamic Huffman
        int hlit  = br.read_bits(5) + 257;
        int hdist = br.read_bits(5) + 1;
        int hclen = br.read_bits(4) + 4;

        int clen_len[19] = {};
        for (int i = 0; i < hclen; ++i)
            clen_len[CLEN_ORDER[i]] = br.read_bits(3);

        HuffmanTree clen_tree;
        clen_tree.build(clen_len, 19);

        int litlen_len[286] = {};
        int dist_len[32] = {};
        int total = hlit + hdist;
        int idx = 0;

        while (idx < total) {
            int sym = clen_tree.decode(br);
            if (sym < 16) {
                if (idx < hlit) litlen_len[idx] = sym;
                else dist_len[idx - hlit] = sym;
                ++idx;
            } else if (sym == 16) {
                if (idx == 0) throw std::runtime_error("inflate: repeat with no previous");
                int prev = idx <= hlit ? litlen_len[idx - 1] : dist_len[idx - hlit - 1];
                int repeat = br.read_bits(2) + 3;
                for (int i = 0; i < repeat && idx < total; ++i, ++idx) {
                    if (idx < hlit) litlen_len[idx] = prev;
                    else dist_len[idx - hlit] = prev;
                }
            } else if (sym == 17) {
                int repeat = br.read_bits(3) + 3;
                for (int i = 0; i < repeat && idx < total; ++i, ++idx) {
                    if (idx < hlit) litlen_len[idx] = 0;
                    else dist_len[idx - hlit] = 0;
                }
            } else if (sym == 18) {
                int repeat = br.read_bits(7) + 11;
                for (int i = 0; i < repeat && idx < total; ++i, ++idx) {
                    if (idx < hlit) litlen_len[idx] = 0;
                    else dist_len[idx - hlit] = 0;
                }
            }
        }

        HuffmanTree litlen_tree, dist_tree;
        litlen_tree.build(litlen_len, hlit);
        dist_tree.build(dist_len, hdist);

        for (;;) {
            int sym = litlen_tree.decode(br);
            if (sym < 256) {
                output.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break;
            } else {
                int code = sym - 257;
                int length = LEN_BASE[code] + br.read_bits(LEN_EXTRA_BITS[code]);
                int dcode = dist_tree.decode(br);
                int distance = DIST_BASE[dcode] + br.read_bits(DIST_EXTRA_BITS[dcode]);
                if (distance > static_cast<int>(output.size()))
                    throw std::runtime_error("inflate: invalid distance");
                size_t src = output.size() - distance;
                for (int i = 0; i < length; ++i)
                    output.push_back(output[src + i]);
            }
        }
        done = bfinal;
        return;
    }

    throw std::runtime_error("inflate: invalid block type " + std::to_string(btype));
}

} // anonymous namespace

std::vector<uint8_t> inflate_raw(std::span<const uint8_t> data) {
    BitReader br(data.data(), data.size());
    std::vector<uint8_t> output;
    output.reserve(data.size() * 4);

    bool done = false;
    while (!done) {
        decompress_block(br, output, done);
    }
    return output;
}

std::vector<uint8_t> inflate_zlib(std::span<const uint8_t> data) {
    if (data.size() < 6) throw std::runtime_error("inflate_zlib: too small");

    uint8_t cmf = data[0];
    uint8_t flg = data[1];
    int cm = cmf & 0xf;
    int cinfo = (cmf >> 4) & 0xf;
    if (cm != 8) throw std::runtime_error("inflate_zlib: not deflate method");
    if (cinfo > 7) throw std::runtime_error("inflate_zlib: invalid window");
    if (((static_cast<int>(cmf) << 8 | flg) % 31) != 0)
        throw std::runtime_error("inflate_zlib: header checksum mismatch");
    if (flg & 0x20) throw std::runtime_error("inflate_zlib: dict not supported");

    return inflate_raw(data.subspan(2, data.size() - 6));
}

} // namespace fpng

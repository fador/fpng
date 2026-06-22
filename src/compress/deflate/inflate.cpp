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

// Canonical Huffman tree with lookup table for fast decoding
struct HuffmanTree {
    std::vector<uint32_t> lookup;
    int max_code_len = 0;

    static uint32_t reverse_bits(uint32_t v, int num_bits) noexcept {
        uint32_t r = 0;
        for (int i = 0; i < num_bits; ++i) {
            r = (r << 1) | (v & 1);
            v >>= 1;
        }
        return r;
    }

    void build(const int* lengths, int num_syms) {
        int bl_count[MAX_BITS + 1] = {};
        max_code_len = 0;
        for (int i = 0; i < num_syms; ++i) {
            if (lengths[i] > 0) {
                bl_count[lengths[i]]++;
                if (lengths[i] > max_code_len) max_code_len = lengths[i];
            }
        }
        bl_count[0] = 0;

        if (max_code_len == 0) {
            lookup.clear();
            return;
        }

        uint16_t next_code[MAX_BITS + 1] = {};
        uint16_t code = 0;
        for (int bits = 1; bits <= MAX_BITS; ++bits) {
            code = static_cast<uint16_t>((code + bl_count[bits - 1]) << 1);
            next_code[bits] = code;
        }

        int table_size = 1 << max_code_len;
        lookup.assign(table_size, 0);

        for (int sym = 0; sym < num_syms; ++sym) {
            int len = lengths[sym];
            if (len > 0) {
                int c = next_code[len];
                next_code[len]++;

                int rev_code = static_cast<int>(reverse_bits(static_cast<uint32_t>(c), len));
                int remaining = max_code_len - len;
                int num_entries = 1 << remaining;

                for (int suffix = 0; suffix < num_entries; ++suffix) {
                    int rev_suffix = static_cast<int>(reverse_bits(static_cast<uint32_t>(suffix), remaining));
                    int idx = rev_code | (rev_suffix << len);
                    if (idx < table_size) {
                        lookup[idx] = (static_cast<uint32_t>(len) << 12) |
                                      static_cast<uint32_t>(sym + 1);
                    }
                }
            }
        }
    }

    int decode(BitReader& br) const {
        if (max_code_len == 0)
            throw std::runtime_error("inflate: empty huffman tree");

        while (br.bits_in_buf < max_code_len) {
            if (br.byte_pos >= br.size)
                throw std::runtime_error("inflate: unexpected EOF");
            br.bit_buf |= static_cast<uint64_t>(br.data[br.byte_pos++]) << br.bits_in_buf;
            br.bits_in_buf += 8;
        }

        int code = static_cast<int>(br.bit_buf & ((1u << max_code_len) - 1));
        uint32_t entry = lookup[static_cast<size_t>(code)];
        int len = static_cast<int>(entry >> 12);
        int sym = static_cast<int>(entry & 0xfff) - 1;

        if (len <= 0 || len > max_code_len || sym < 0)
            throw std::runtime_error("inflate: invalid huffman code");

        br.bit_buf >>= len;
        br.bits_in_buf -= len;
        return sym;
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

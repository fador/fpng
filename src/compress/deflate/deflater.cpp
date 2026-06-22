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
    parse_opts.use_bt_match = opts.bt_match_finder;
    
    // Iterative refinement
    LZ77Parser::CostModel cm;
    int iters = std::max(1, opts.iterations);
    std::vector<LZ77Parser::Token> tokens;

    for (int iter = 0; iter < iters; ++iter) {
        if (iter == 0) {
            parse_opts.optimal = false;
            parse_opts.lazy_matching = true;
        } else {
            parse_opts.optimal = true;
            parse_opts.cost_model = cm;
        }
        tokens = parser.parse(data, size, parse_opts);

        if (iter + 1 < iters) {
            // Count frequencies from current tokens
            uint32_t ll_freq[deflate::MAX_LITLEN_SYMS] = {};
            uint32_t d_freq[deflate::MAX_DIST_SYMS] = {};
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                auto& t = tokens[i];
                if (t.type == LZ77Parser::Token::LITERAL) {
                    ll_freq[t.literal]++;
                } else {
                    ll_freq[257 + deflate::length_code(t.match_length)]++;
                    d_freq[deflate::distance_code(t.match_distance)]++;
                }
            }
            ll_freq[256] = 1;

            // Build cost model (use fixed Huffman lengths)
            static uint8_t fixed_ll[288], fixed_d[32];
            static bool inited = false;
            if (!inited) {
                for (int i = 0; i <= 143; ++i) fixed_ll[i] = 8;
                for (int i = 144; i <= 255; ++i) fixed_ll[i] = 9;
                for (int i = 256; i <= 279; ++i) fixed_ll[i] = 7;
                for (int i = 280; i <= 287; ++i) fixed_ll[i] = 8;
                for (int i = 0; i < 32; ++i) fixed_d[i] = 5;
                inited = true;
            }
            cm.litlen_lengths = fixed_ll;
            cm.dist_lengths = fixed_d;
        }
    }

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

// Encode using dynamic Huffman blocks (BTYPE=2)
void write_dynamic_block(const uint8_t* data, size_t size,
                          bool is_last, const DeflateOptions& opts,
                          std::vector<uint8_t>& out) {
    // Iterative refinement: parse, build Huffman, parse again with costs, repeat
    LZ77Parser parser;
    LZ77Parser::Options parse_opts;
    int iters = std::max(1, opts.iterations);
    std::vector<LZ77Parser::Token> tokens;
    std::vector<uint8_t> ll_len, d_len;

    for (int iter = 0; iter < iters; ++iter) {
        if (iter == 0) {
            parse_opts.optimal = false;
            parse_opts.lazy_matching = true;
        } else {
            parse_opts.optimal = true;
            LZ77Parser::CostModel cm;
            cm.litlen_lengths = ll_len.data();
            cm.dist_lengths = d_len.data();
            parse_opts.cost_model = cm;
        }
        tokens = parser.parse(data, size, parse_opts);

        // Count frequencies and build Huffman trees for next iteration
        uint32_t ll_freq[deflate::MAX_LITLEN_SYMS] = {};
        uint32_t d_freq[deflate::MAX_DIST_SYMS] = {};
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (t.type == LZ77Parser::Token::LITERAL) {
                ll_freq[t.literal]++;
            } else {
                ll_freq[257 + deflate::length_code(t.match_length)]++;
                d_freq[deflate::distance_code(t.match_distance)]++;
            }
        }
        ll_freq[deflate::END_OF_BLOCK] = 1;

        ll_len = HuffmanEncoder::compute_lengths(ll_freq, deflate::MAX_LITLEN_SYMS, 15);
        d_len = HuffmanEncoder::compute_lengths(d_freq, deflate::MAX_DIST_SYMS, 15);
    }

     // Now we have the final tokens and Huffman trees
    // Edge case: no frequencies
    bool has_any = false;
    for (auto v : ll_len) { if (v > 0) { has_any = true; break; } }
    if (!has_any) {
        write_stored_block(data, size, is_last, out);
        return;
    }

    auto ll_code = HuffmanEncoder::lengths_to_codes(ll_len.data(), deflate::MAX_LITLEN_SYMS);
    auto d_code = HuffmanEncoder::lengths_to_codes(d_len.data(), deflate::MAX_DIST_SYMS);

    // Determine HLIT, HDIST
    int hlit = 286;
    while (hlit > 257 && ll_len[hlit - 1] == 0) --hlit;
    int hdist = 30;
    while (hdist > 1 && d_len[hdist - 1] == 0) --hdist;

    // Step 4: RLE encode the tree description
    std::vector<uint8_t> tree_rle;
    // Write each code length, with RLE for repeats
    auto rle = [&](const uint8_t* lengths, int count) {
        int i = 0;
        while (i < count) {
            uint8_t len = lengths[i];
            if (len == 0) {
                int run = 0;
                while (i + run < count && lengths[i + run] == 0) ++run;
                if (run < 3) {
                    for (int r = 0; r < run; ++r) tree_rle.push_back(0);
                } else if (run <= 10) {
                    tree_rle.push_back(17);
                    tree_rle.push_back(static_cast<uint8_t>(run - 3));
                } else {
                    int n = std::min(run, 138);
                    tree_rle.push_back(18);
                    tree_rle.push_back(static_cast<uint8_t>(n - 11));
                    i += n;
                    continue;
                }
                i += run;
            } else {
                tree_rle.push_back(len);
                ++i;
                int run = 0;
                while (i + run < count && lengths[i + run] == len) ++run;
                if (run >= 3) {
                    int n = std::min(run, 6);
                    tree_rle.push_back(16);
                    tree_rle.push_back(static_cast<uint8_t>(n - 3));
                    i += n;
                }
            }
        }
    };
    rle(ll_len.data(), hlit);
    rle(d_len.data(), hdist);

    // Step 5: Build CLEN tree from tree_rle frequencies
    uint32_t clen_freq[deflate::MAX_CLEN_SYMS] = {};
    for (size_t i = 0; i < tree_rle.size(); ++i) {
        uint8_t v = tree_rle[i];
        if (v < 16) clen_freq[v]++;
        else { clen_freq[v]++; i++; } // skip extra bits byte
    }

    auto clen_len = HuffmanEncoder::compute_lengths(clen_freq, deflate::MAX_CLEN_SYMS, 7);
    auto clen_code = HuffmanEncoder::lengths_to_codes(clen_len.data(), deflate::MAX_CLEN_SYMS);

    // Determine HCLEN
    int hclen = 19;
    while (hclen > 4 && clen_len[deflate::CLEN_ORDER[hclen - 1]] == 0)
        --hclen;

    // Step 6: Write the block
    BitWriter bw;
    bw.write_bits(is_last ? 1 : 0, 1);        // BFINAL
    bw.write_bits(2, 2);                       // BTYPE = dynamic
    bw.write_bits(hlit - 257, 5);              // HLIT
    bw.write_bits(hdist - 1, 5);               // HDIST
    bw.write_bits(hclen - 4, 4);               // HCLEN

    // CLEN code lengths
    for (int i = 0; i < hclen; ++i) {
        int idx = deflate::CLEN_ORDER[i];
        bw.write_bits(clen_len[idx], 3);
    }

    // Tree RLE data encoded with CLEN codes
    for (size_t i = 0; i < tree_rle.size(); ++i) {
        uint8_t v = tree_rle[i];
        auto& c = clen_code[v];
        bw.write_bits(reverse_bits_u32(c.code, c.bits), c.bits);

        if (v == 16) { ++i; bw.write_bits(tree_rle[i], 2); }
        else if (v == 17) { ++i; bw.write_bits(tree_rle[i], 3); }
        else if (v == 18) { ++i; bw.write_bits(tree_rle[i], 7); }
    }

    // Compressed data
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        auto& t = tokens[i];
        if (t.type == LZ77Parser::Token::LITERAL) {
            auto& c = ll_code[t.literal];
            bw.write_bits(reverse_bits_u32(c.code, c.bits), c.bits);
        } else {
            int lc = deflate::length_code(t.match_length);
            auto& lc_ = ll_code[257 + lc];
            bw.write_bits(reverse_bits_u32(lc_.code, lc_.bits), lc_.bits);
            int le = deflate::length_extra_bits(lc);
            if (le) bw.write_bits(t.match_length - deflate::length_base(lc), le);

            int dc = deflate::distance_code(t.match_distance);
            auto& dc_ = d_code[dc];
            bw.write_bits(reverse_bits_u32(dc_.code, dc_.bits), dc_.bits);
            int de = deflate::distance_extra_bits(dc);
            if (de) bw.write_bits(t.match_distance - deflate::distance_base(dc), de);
        }
    }

    // EOB
    auto& eob = ll_code[deflate::END_OF_BLOCK];
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
        else if (opts.level >= CompressionLevel::Best)
            write_dynamic_block(data.data() + b.start_offset,
                               b.end_offset - b.start_offset,
                               is_last, opts, out);
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

#include "compress/deflate/deflater.hpp"
#include "compress/deflate/huffman.hpp"
#include "compress/deflate/block_splitter.hpp"
#include "compress/deflate/bit_writer.hpp"

#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <vector>

namespace fpng {

namespace {

// floor(log2(x)) for x >= 1.
static int ilog2_floor(uint64_t x) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64)
    unsigned long idx = 0;
    _BitScanReverse64(&idx, x);
    return static_cast<int>(idx);
#else
    return 63 - __builtin_clzll(x);
#endif
}

// log2(x) in Q16 fixed point (16 fractional bits), for x >= 1, x <= 2^64.
// Uses a Q31 mantissa with the squaring / bit-extraction method; no float math.
static uint32_t log2_fixed(uint64_t x) {
    int exp = ilog2_floor(x);
    // mantissa m = x / 2^exp in [1,2), represented as Q31 (value = m/2^31).
    uint64_t m = (exp <= 31) ? (x << (31 - exp)) : (x >> (exp - 31));
    // m is in [2^31, 2^32).
    uint32_t frac = 0;
    uint64_t cur = m;
    for (int i = 0; i < 16; ++i) {
        cur = (cur * cur) >> 31; // Q31
        frac <<= 1;
        if (cur >= (1ULL << 32)) { cur >>= 1; frac |= 1; }
    }
    return (static_cast<uint32_t>(exp) << 16) | frac;
}

// Compute entropy-based costs from symbol frequencies.
// Returns a 288+32 entry vector of scaled fixed-point costs (scale=1024).
// cost[sym] = ceil(-log2(freq[sym] / total) * 1024)
// Unused symbols get a fallback cost (seeded with freq=1) instead of 65535
// to prevent the DP parser from being locked out of favorable matches.
std::vector<uint16_t> compute_entropy_costs(
    const uint32_t* ll_freq, const uint32_t* d_freq) {

    std::vector<uint16_t> costs(288 + 32, 0);

    // Compute total literal/length frequency
    uint64_t ll_total = 0;
    for (int i = 0; i < 288; ++i) ll_total += ll_freq[i];
    if (ll_total == 0) ll_total = 1;
    uint32_t ll_total_log = log2_fixed(ll_total);

    // Compute total distance frequency, seeding unused codes with freq=1
    uint64_t d_total = 0;
    for (int i = 0; i < 32; ++i) {
        if (d_freq[i] > 0) d_total += d_freq[i];
        else d_total += 1;
    }
    if (d_total == 0) d_total = 1;
    uint32_t d_total_log = log2_fixed(d_total);

    // cost = ceil((log2(total) - log2(freq)) * 1024). The Q16 log yields
    // (diff * 1024) = diff * 2^10; dividing by 2^6 gives diff * 1024 / 2^16.
    auto entropy_cost = [](uint64_t total_log, uint64_t freq) -> uint16_t {
        uint32_t diff = static_cast<uint32_t>(total_log) - log2_fixed(freq);
        uint32_t c = (diff + 63) >> 6; // ceil(diff / 64)
        if (c == 0) c = 1;             // minimum 1/1024 bit
        return c > 65535 ? 65535 : static_cast<uint16_t>(c);
    };

    for (int i = 0; i < 288; ++i) {
        if (ll_freq[i] > 0) {
            costs[i] = entropy_cost(ll_total_log, ll_freq[i]);
        } else {
            costs[i] = 65535; // literal/length: keep frozen (256 literal values, most appear)
        }
    }

    for (int i = 0; i < 32; ++i) {
        int idx = 288 + i;
        uint32_t freq = d_freq[i] > 0 ? d_freq[i] : 1; // seed unused with 1
        costs[idx] = entropy_cost(d_total_log, freq);
    }

    return costs;
}

// Forward declarations
void write_stored_block(BitWriter& bw, const uint8_t* data, size_t size,
                         bool is_last);
void write_fixed_block(BitWriter& bw, const uint8_t* full_data, size_t full_size,
                        size_t start, size_t size, bool is_last,
                        const DeflateOptions& opts, const MatchFinder* mf);
void write_dynamic_block(BitWriter& bw, const uint8_t* full_data, size_t full_size,
                          size_t start, size_t size, bool is_last,
                          const DeflateOptions& opts, const MatchFinder* mf);

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
void write_fixed_block(BitWriter& bw, const uint8_t* full_data, size_t full_size,
                        size_t start, size_t size, bool is_last,
                        const DeflateOptions& opts, const MatchFinder* mf) {
    LZ77Parser parser;
    LZ77Parser::Options parse_opts;
    parse_opts.chain_depth = opts.chain_depth;
    
    // Iterative refinement
    LZ77Parser::CostModel cm;
    int iters = std::max(1, opts.iterations);
    std::vector<LZ77Parser::Token> tokens;

    for (int iter = 0; iter < iters; ++iter) {
        if (iter == 0) {
            parse_opts.optimal = false;
            parse_opts.lazy_matching = true;
            // Row stride supplied by the caller (filtered scanline + 1); the
            // match finder probes this distance for between-row matches.
            parse_opts.row_stride = opts.row_stride;
        } else {
            parse_opts.optimal = true;
            // Use actual fixed Huffman costs, not entropy estimates.
            // Fixed Huffman: litlen 0-143=8, 144-255=9, EOB=7, 257-279=7,
            // 280-285=8, distance=5. These are the costs the encoder uses.
            static std::vector<uint16_t> fixed_costs;
            if (fixed_costs.empty()) {
                fixed_costs.resize(288 + 32);
                for (int i = 0; i <= 143; ++i) fixed_costs[i] = 8 * 1024;
                for (int i = 144; i <= 255; ++i) fixed_costs[i] = 9 * 1024;
                fixed_costs[256] = 7 * 1024; // EOB
                for (int i = 257; i <= 279; ++i) fixed_costs[i] = 7 * 1024;
                for (int i = 280; i <= 285; ++i) fixed_costs[i] = 8 * 1024;
                for (int i = 0; i < 32; ++i) fixed_costs[288 + i] = 5 * 1024;
            }
            cm.precomputed_costs = fixed_costs.data();
            parse_opts.cost_model = cm;
        }
        tokens = parser.parse_range(full_data, full_size, start, start + size,
                                    parse_opts, mf);
    }

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
}

// Encode using dynamic Huffman blocks (BTYPE=2)
void write_dynamic_block(BitWriter& bw, const uint8_t* full_data, size_t full_size,
                           size_t start, size_t size, bool is_last,
                           const DeflateOptions& opts, const MatchFinder* mf) {
    // Iterative refinement: parse, build Huffman, parse again with costs, repeat
    LZ77Parser parser;
    LZ77Parser::Options parse_opts;
    int iters = std::max(1, opts.iterations);
    std::vector<LZ77Parser::Token> tokens;
    std::vector<uint8_t> ll_len, d_len;
    // Store entropy costs for the CostModel (recomputed each iteration)
    std::vector<uint16_t> entropy_costs;

    for (int iter = 0; iter < iters; ++iter) {
        if (iter == 0) {
            parse_opts.optimal = false;
            // Lazy matching is counterproductive at deep chain depths
            parse_opts.lazy_matching = true;
            parse_opts.row_stride = opts.row_stride;
        } else {
            parse_opts.optimal = true;
            LZ77Parser::CostModel cm;
            cm.precomputed_costs = entropy_costs.data();
            parse_opts.cost_model = cm;
        }
        tokens = parser.parse_range(full_data, full_size, start, start + size,
                                    parse_opts, mf);

        // Count frequencies
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

        // Compute entropy costs for next iteration's CostModel
        entropy_costs = compute_entropy_costs(ll_freq, d_freq);

        // Build Huffman trees (needed for final encoding)
        ll_len = HuffmanEncoder::compute_lengths(ll_freq, deflate::MAX_LITLEN_SYMS, 15);
        d_len = HuffmanEncoder::compute_lengths(d_freq, deflate::MAX_DIST_SYMS, 15);
    }

     // Now we have the final tokens and Huffman trees
    // Edge case: no frequencies
    bool has_any = false;
    for (auto v : ll_len) { if (v > 0) { has_any = true; break; } }
    if (!has_any) {
        write_stored_block(bw, full_data + start, size, is_last);
        return;
    }

    auto ll_code = HuffmanEncoder::lengths_to_codes(ll_len.data(), deflate::MAX_LITLEN_SYMS);
    auto d_code = HuffmanEncoder::lengths_to_codes(d_len.data(), deflate::MAX_DIST_SYMS);

    // Determine HLIT, HDIST
    int hlit = 286;
    while (hlit > 257 && ll_len[hlit - 1] == 0) --hlit;
    int hdist = 30;
    while (hdist > 1 && d_len[hdist - 1] == 0) --hdist;

    // Step 4-5: Build CLEN tree from the code-length frequencies directly.
    std::vector<uint8_t> tree_rle;
    uint32_t clen_freq[deflate::MAX_CLEN_SYMS] = {};
    for (int i = 0; i < hlit; ++i) clen_freq[ll_len[i]]++;
    for (int i = 0; i < hdist; ++i) clen_freq[d_len[i]]++;
    clen_freq[16] = std::max(clen_freq[16], 1u);
    clen_freq[17] = std::max(clen_freq[17], 1u);
    clen_freq[18] = std::max(clen_freq[18], 1u);

    auto clen_len = HuffmanEncoder::compute_lengths(clen_freq, deflate::MAX_CLEN_SYMS, 7);
    auto clen_code = HuffmanEncoder::lengths_to_codes(clen_len.data(), deflate::MAX_CLEN_SYMS);
    int hclen = 19;
    while (hclen > 4 && clen_len[deflate::CLEN_ORDER[hclen - 1]] == 0)
        --hclen;

    // Re-encode tree RLE with optimal thresholds using actual CLEN costs.
    auto optimal_rle = [&](const uint8_t* lengths, int count) {
            int i = 0;
            while (i < count) {
                uint8_t len = lengths[i];
                if (len == 0) {
                    int run = 0;
                    while (i + run < count && lengths[i + run] == 0) ++run;

                    // Compute costs for each encoding choice
                    int cost_individual = run * clen_len[0];                // individual zeros
                    int cost_17 = clen_len[17] + 3;                          // repeat 3-10
                    int cost_18 = clen_len[18] + 7;                          // repeat 11-138

                    // Greedy optimal: pick cheapest for each segment
                    int processed = 0;
                    while (processed < run) {
                        int remaining = run - processed;
                        if (remaining >= 11 && cost_18 <= cost_individual * std::min(remaining, 138)) {
                            int n = std::min(remaining, 138);
                            tree_rle.push_back(18);
                            tree_rle.push_back(static_cast<uint8_t>(n - 11));
                            processed += n;
                        } else if (remaining >= 3 && cost_17 <= cost_individual * std::min(remaining, 10)) {
                            int n = std::min(remaining, 10);
                            tree_rle.push_back(17);
                            tree_rle.push_back(static_cast<uint8_t>(n - 3));
                            processed += n;
                        } else {
                            tree_rle.push_back(0);
                            processed++;
                        }
                    }
                    i += run;
                } else {
                    tree_rle.push_back(len);
                    ++i;
                    int run = 0;
                    while (i + run < count && lengths[i + run] == len) ++run;
                    if (run >= 3) {
                        // Code 16: repeat previous length 3-6 times
                        int cost_16 = clen_len[16] + 2;
                        int n = std::min(run, 6);
                        if (cost_16 <= n * clen_len[len]) {
                            tree_rle.push_back(16);
                            tree_rle.push_back(static_cast<uint8_t>(n - 3));
                            i += n;
                        }
                    }
                }
            }
        };

    // Rebuild tree_rle with optimal encoding
    tree_rle.clear();
    optimal_rle(ll_len.data(), hlit);
    optimal_rle(d_len.data(), hdist);

    // Recompute CLEN frequencies from optimized RLE
    std::memset(clen_freq, 0, sizeof(clen_freq));
    for (size_t i = 0; i < tree_rle.size(); ++i) {
        uint8_t v = tree_rle[i];
        if (v < 16) clen_freq[v]++;
        else { clen_freq[v]++; i++; }
    }

    // Rebuild CLEN tree with new frequencies
    clen_len = HuffmanEncoder::compute_lengths(clen_freq, deflate::MAX_CLEN_SYMS, 7);
    clen_code = HuffmanEncoder::lengths_to_codes(clen_len.data(), deflate::MAX_CLEN_SYMS);

    // Recompute HCLEN
    hclen = 19;
    while (hclen > 4 && clen_len[deflate::CLEN_ORDER[hclen - 1]] == 0)
        --hclen;

    // Step 6: Write the block
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
}

void write_stored_block(BitWriter& bw, const uint8_t* data, size_t size,
                         bool is_last) {
    bw.write_bits(is_last ? 1 : 0, 1);
    bw.write_bits(0, 2);
    // A stored block must start on a byte boundary. Emit the current partial
    // byte (containing the preceding block's trailing bits plus these header
    // bits) padded with zeros; the decoder reads the header then discards the
    // rest of that byte before LEN/NLEN.
    bw.flush_to_byte();

    uint16_t len = static_cast<uint16_t>(size);
    bw.write_byte(static_cast<uint8_t>(len & 0xff));
    bw.write_byte(static_cast<uint8_t>((len >> 8) & 0xff));
    bw.write_byte(static_cast<uint8_t>((~len) & 0xff));
    bw.write_byte(static_cast<uint8_t>(((~len) >> 8) & 0xff));
    for (size_t i = 0; i < size; ++i)
        bw.write_byte(data[i]);
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

    // One continuous bit stream spans every block. Padding to a byte boundary
    // is only applied at the very end (and internally at stored-block starts);
    // flushing between Huffman blocks would insert spurious bits.
    BitWriter bw;

    if (opts.level == CompressionLevel::Store) {
        size_t pos = 0;
        while (pos < data.size()) {
            size_t sz = std::min(data.size() - pos, size_t(65535));
            write_stored_block(bw, data.data() + pos, sz,
                               pos + sz >= data.size());
            pos += sz;
        }
        bw.flush_to_byte();
        std::vector<uint8_t> out(bw.bytes().begin(),
                                 bw.bytes().begin() + bw.byte_count());
        return out;
    }

    // Auto-scale: for large images (>=128K bytes filtered), use 8192-byte
    // blocks to specialize Huffman trees per data region. For smaller
    // images, use a single 65536-byte block to minimize tree overhead.
    // Never exceed 65535 so every block remains eligible for a stored block.
    size_t eff_block_size = opts.max_block_size;
    if (eff_block_size == 0) {
        eff_block_size = (data.size() >= 131072) ? 8192 : 65536;
    }
    eff_block_size = std::min(eff_block_size, size_t(65535));
    auto blocks = opts.adaptive_blocks
        ? BlockSplitter::split_greedy_adaptive(data.data(), data.size(),
                                                4096, eff_block_size)
        : BlockSplitter::split(data.data(), data.size(), eff_block_size);

    if (blocks.empty())
        blocks.push_back({0, data.size()});

    // Version with explicit opts
    DeflateOptions adjusted = opts;
    if (adjusted.chain_depth == 0) {
        switch (adjusted.level) {
            case CompressionLevel::Fast:    adjusted.chain_depth = 128;  break;
            case CompressionLevel::Default: adjusted.chain_depth = 1024; break;
            case CompressionLevel::Best:    adjusted.chain_depth = 2048; break;
            case CompressionLevel::Ultra:   adjusted.chain_depth = 8192; break;
            default: adjusted.chain_depth = 128; break;
        }
    }

    // One match finder spans the entire stream so that matches may reference
    // data in earlier blocks (the DEFLATE sliding window persists across
    // block boundaries). Rebuilding it per block truncated the window.
    MatchFinder shared_mf;
    shared_mf.chain_depth = adjusted.chain_depth;
    shared_mf.nice_len = 32;
    shared_mf.row_stride = adjusted.row_stride;
    shared_mf.init(data.data(), data.size());

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        auto& b = blocks[bi];
        bool is_last = (bi == blocks.size() - 1);
        size_t block_size = b.end_offset - b.start_offset;

        // Short blocks: dynamic Huffman overhead exceeds savings.
        // Use stored for very small blocks (<256), but for blocks 256-512,
        // check byte entropy: low entropy means dynamic Huffman wins despite
        // tree overhead (a block of all-zeros compresses to ~15 bytes stored
        // but could be ~10 bytes dynamic).
        bool use_stored = false;
        if (adjusted.level < CompressionLevel::Ultra) {
            if (block_size < 256) {
                use_stored = true;
            } else if (block_size < 512) {
                // Compute byte entropy; if data is highly repetitive, try dynamic
                uint32_t freq[256] = {};
                for (size_t i = 0; i < block_size; ++i)
                    freq[data[b.start_offset + i]]++;
                double ent = 0;
                double inv = 1.0 / block_size;
                for (int i = 0; i < 256; ++i) {
                    if (freq[i] > 0) {
                        double p = freq[i] * inv;
                        ent -= p * std::log2(p);
                    }
                }
                // Entropy < 3 bits/byte → data is compressible enough to justify tree overhead
                use_stored = (ent >= 3.0);
            }
        }

        if (use_stored)
            write_stored_block(bw, data.data() + b.start_offset, block_size,
                               is_last);
        else if (adjusted.level >= CompressionLevel::Default)
            write_dynamic_block(bw, data.data(), data.size(),
                                b.start_offset, block_size,
                                is_last, adjusted, &shared_mf);
        else
            write_fixed_block(bw, data.data(), data.size(),
                              b.start_offset, block_size,
                              is_last, adjusted, &shared_mf);
    }

    bw.flush_to_byte();
    return std::vector<uint8_t>(bw.bytes().begin(),
                                bw.bytes().begin() + bw.byte_count());
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

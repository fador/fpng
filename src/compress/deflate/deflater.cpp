#include "compress/deflate/deflater.hpp"
#include "compress/deflate/huffman.hpp"
#include "compress/deflate/block_splitter.hpp"
#include "compress/deflate/bit_writer.hpp"

#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <vector>
#include <limits>

namespace fpng {

namespace {

// Forward declarations
void write_stored_block(BitWriter& bw, const uint8_t* data, size_t size,
                         bool is_last);
void encode_fixed_tokens(BitWriter& bw,
                         const std::vector<LZ77Parser::Token>& tokens,
                         size_t begin, size_t end, bool is_last);
void encode_dynamic_tokens(BitWriter& bw,
                           const std::vector<LZ77Parser::Token>& tokens,
                           size_t begin, size_t end, bool is_last);

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
// Encode a fixed-Huffman block from a precomputed token range [begin,end).
void encode_fixed_tokens(BitWriter& bw,
                         const std::vector<LZ77Parser::Token>& tokens,
                         size_t begin, size_t end, bool is_last) {
    bw.write_bits(is_last ? 1 : 0, 1);  // BFINAL
    bw.write_bits(1, 2);                 // BTYPE = fixed Huffman

    for (size_t i = begin; i < end; ++i) {
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
// Encode a dynamic-Huffman block from a precomputed token range [begin,end).
// The whole stream is parsed once globally (so matches may span block
// boundaries); only the Huffman trees are per block.
void encode_dynamic_tokens(BitWriter& bw,
                           const std::vector<LZ77Parser::Token>& tokens,
                           size_t begin, size_t end, bool is_last) {
    // Count frequencies over this block's tokens.
    uint32_t ll_freq[deflate::MAX_LITLEN_SYMS] = {};
    uint32_t d_freq[deflate::MAX_DIST_SYMS] = {};
    for (size_t i = begin; i < end; ++i) {
        auto& t = tokens[i];
        if (t.type == LZ77Parser::Token::LITERAL) {
            ll_freq[t.literal]++;
        } else {
            ll_freq[257 + deflate::length_code(t.match_length)]++;
            d_freq[deflate::distance_code(t.match_distance)]++;
        }
    }
    ll_freq[deflate::END_OF_BLOCK] = 1;

    auto ll_len = HuffmanEncoder::compute_lengths(ll_freq, deflate::MAX_LITLEN_SYMS, 15);
    auto d_len = HuffmanEncoder::compute_lengths(d_freq, deflate::MAX_DIST_SYMS, 15);

    bool has_any = false;
    for (auto v : ll_len) { if (v > 0) { has_any = true; break; } }
    if (!has_any) return;

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
    for (size_t i = begin; i < end; ++i) {
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

// Choose DEFLATE block boundaries from a tokenization of the whole stream.
// Unlike a byte-entropy heuristic, this estimates the real encoded cost of
// each candidate block (Huffman symbol entropy + match extra bits + a tree
// overhead term) and minimizes the total with a DP. Boundaries can only fall
// on token boundaries, so matches are never split.
std::vector<std::pair<size_t, size_t>> token_aware_split(
    const std::vector<LZ77Parser::Token>& toks, size_t size,
    size_t min_block, size_t max_block) {

    constexpr size_t LITLEN = deflate::MAX_LITLEN_SYMS; // 288
    constexpr size_t DIST   = deflate::MAX_DIST_SYMS;   // 32
    constexpr size_t NSYM   = LITLEN + DIST;

    if (toks.size() < 2) return {{0, toks.empty() ? 0 : toks.size() - 1}};
    const size_t real = toks.size() - 1; // exclude EOB sentinel

    // Byte offset at each token boundary.
    std::vector<size_t> off(real + 1, 0);
    for (size_t i = 0; i < real; ++i) {
        size_t blen = (toks[i].type == LZ77Parser::Token::LITERAL)
                          ? 1 : toks[i].match_length;
        off[i + 1] = off[i] + blen;
    }
    if (off[real] != size) return {{0, real}}; // parse mismatch; bail out

    // Candidate boundaries at ~step-byte intervals (capped count).
    size_t step = 4096;
    if (size / step > 1024) step = size / 1024;
    std::vector<size_t> cand;
    cand.push_back(0);
    size_t next = step;
    for (size_t i = 1; i < real; ++i) {
        if (off[i] >= next) { cand.push_back(i); next = off[i] + step; }
    }
    cand.push_back(real);
    const size_t C = cand.size();
    if (C < 2) return {{0, real}};

    // Prefix symbol frequencies and extra-bit counts at each candidate.
    std::vector<std::vector<uint32_t>> pref(C, std::vector<uint32_t>(NSYM, 0));
    std::vector<uint64_t> extra_pref(C, 0);
    {
        std::vector<uint32_t> cur(NSYM, 0);
        uint64_t extra = 0;
        size_t ci = 1;
        for (size_t i = 0; i < real; ++i) {
            auto& t = toks[i];
            if (t.type == LZ77Parser::Token::LITERAL) {
                cur[t.literal]++;
            } else {
                int lc = deflate::length_code(t.match_length);
                int dc = deflate::distance_code(t.match_distance);
                cur[257 + lc]++;
                cur[LITLEN + dc]++;
                extra += deflate::length_extra_bits(lc) +
                         deflate::distance_extra_bits(dc);
            }
            if (ci < C && i + 1 == cand[ci]) {
                pref[ci] = cur;
                extra_pref[ci] = extra;
                ++ci;
            }
        }
    }

    auto block_cost_bits = [&](size_t a, size_t b) -> double {
        uint32_t ll_freq[LITLEN] = {};
        uint32_t d_freq[DIST] = {};
        const auto& pa = pref[a];
        const auto& pb = pref[b];
        for (size_t s = 0; s < LITLEN; ++s) ll_freq[s] = pb[s] - pa[s];
        for (size_t s = 0; s < DIST; ++s) d_freq[s] = pb[LITLEN + s] - pa[LITLEN + s];
        ll_freq[deflate::END_OF_BLOCK] =
            std::max(ll_freq[deflate::END_OF_BLOCK], 1u);

        auto ll_len = HuffmanEncoder::compute_lengths(ll_freq, LITLEN, 15);
        auto d_len = HuffmanEncoder::compute_lengths(d_freq, DIST, 15);
        double bits = 0;
        int active = 0;
        for (size_t s = 0; s < LITLEN; ++s) {
            bits += static_cast<double>(ll_freq[s]) * ll_len[s];
            if (ll_len[s]) ++active;
        }
        for (size_t s = 0; s < DIST; ++s) {
            bits += static_cast<double>(d_freq[s]) * d_len[s];
            if (d_len[s]) ++active;
        }
        bits += static_cast<double>(extra_pref[b] - extra_pref[a]);
        // Dynamic header + code-length tree overhead approximation.
        bits += 60.0 + 6.0 * active;
        return bits;
    };

    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> best(C, INF);
    std::vector<int> prev(C, -1);
    best[0] = 0;
    for (size_t j = 1; j < C; ++j) {
        for (size_t i = j; i-- > 0;) {
            size_t seg = off[cand[j]] - off[cand[i]];
            if (seg < min_block) continue;
            if (seg > max_block) break;
            if (best[i] == INF) continue;
            double c = best[i] + block_cost_bits(i, j);
            if (c < best[j]) { best[j] = c; prev[j] = static_cast<int>(i); }
        }
    }

    std::vector<size_t> cuts; // token indices
    int idx = static_cast<int>(C - 1);
    while (idx > 0 && prev[idx] >= 0) {
        cuts.push_back(cand[idx]);
        idx = prev[idx];
    }
    cuts.push_back(0);
    std::reverse(cuts.begin(), cuts.end());

    std::vector<std::pair<size_t, size_t>> ranges;
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
        ranges.push_back({cuts[i], cuts[i + 1]});
    if (ranges.empty()) ranges.push_back({0, real});
    return ranges;
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

    // Auto-scale: a single 65535-byte block minimizes tree overhead; larger
    // streams are split where the estimated Huffman cost actually drops.
    size_t eff_block_size = opts.max_block_size;
    if (eff_block_size == 0) {
        eff_block_size = 65535;
    }
    eff_block_size = std::min(eff_block_size, size_t(65535));

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

    // Global tokenization with iterative refinement. Parsing the whole stream
    // once (instead of per block) lets matches span block boundaries; only the
    // Huffman trees are per block.
    std::vector<LZ77Parser::Token> tokens;
    {
        LZ77Parser parser;
        LZ77Parser::Options popts;
        popts.min_match = deflate::MIN_MATCH_LEN;
        popts.lazy_matching = true;
        popts.row_stride = adjusted.row_stride;
        int iters = std::max(1, adjusted.iterations);
        std::vector<uint8_t> ll_len, d_len;
        std::vector<uint16_t> actual_costs;
        for (int iter = 0; iter < iters; ++iter) {
            if (iter == 0) {
                popts.optimal = false;
            } else {
                popts.optimal = true;
                LZ77Parser::CostModel cm;
                cm.precomputed_costs = actual_costs.data();
                popts.cost_model = cm;
            }
            tokens = parser.parse_range(data.data(), data.size(), 0, data.size(),
                                        popts, &shared_mf);

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
            actual_costs.assign(deflate::MAX_LITLEN_SYMS + deflate::MAX_DIST_SYMS, 0);
            for (int s = 0; s < deflate::MAX_LITLEN_SYMS; ++s)
                actual_costs[s] = static_cast<uint16_t>(
                    (ll_len[s] ? ll_len[s] : deflate::MAX_BITS) << 10);
            for (int s = 0; s < deflate::MAX_DIST_SYMS; ++s)
                actual_costs[deflate::MAX_LITLEN_SYMS + s] = static_cast<uint16_t>(
                    (d_len[s] ? d_len[s] : deflate::MAX_BITS) << 10);
        }
    }

    const size_t real = tokens.empty() ? 0 : tokens.size() - 1; // exclude EOB
    // Byte offset at each token boundary (used for stored-block sizing).
    std::vector<size_t> off(real + 1, 0);
    for (size_t i = 0; i < real; ++i)
        off[i + 1] = off[i] + ((tokens[i].type == LZ77Parser::Token::LITERAL)
                                   ? 1 : tokens[i].match_length);

    // Choose token ranges (blocks).
    std::vector<std::pair<size_t, size_t>> ranges;
    if (opts.adaptive_blocks) {
        ranges = token_aware_split(tokens, data.size(), 4096, eff_block_size);
    } else {
        auto bblocks = BlockSplitter::split(data.data(), data.size(), eff_block_size);
        size_t ti = 0;
        for (auto& b : bblocks) {
            size_t tb = ti;
            while (ti < real && off[ti] < b.end_offset) ++ti;
            ranges.push_back({tb, ti});
        }
        if (ranges.empty()) ranges.push_back({0, real});
        ranges.back().second = real;
    }

    // Drop empty ranges so is_last lands on a real block.
    std::vector<std::pair<size_t, size_t>> emit;
    for (auto& r : ranges)
        if (r.second > r.first) emit.push_back(r);

    for (size_t ri = 0; ri < emit.size(); ++ri) {
        size_t tb = emit[ri].first;
        size_t te = emit[ri].second;
        bool is_last = (ri == emit.size() - 1);
        size_t start = (tb < off.size()) ? off[tb] : data.size();
        size_t endb = (te < off.size()) ? off[te] : data.size();
        size_t block_size = endb - start;

        // Short blocks: dynamic Huffman overhead exceeds savings. Use stored
        // for very small blocks, or for 256-512 byte blocks whose byte entropy
        // is high (incompressible).
        bool use_stored = false;
        if (adjusted.level < CompressionLevel::Ultra) {
            if (block_size < 256) {
                use_stored = true;
            } else if (block_size < 512) {
                uint32_t freq[256] = {};
                for (size_t i = start; i < endb; ++i)
                    freq[data[i]]++;
                double ent = 0;
                double inv = 1.0 / block_size;
                for (int i = 0; i < 256; ++i) {
                    if (freq[i] > 0) {
                        double p = freq[i] * inv;
                        ent -= p * std::log2(p);
                    }
                }
                use_stored = (ent >= 3.0);
            }
        }

        if (use_stored)
            write_stored_block(bw, data.data() + start, block_size, is_last);
        else if (adjusted.level >= CompressionLevel::Default)
            encode_dynamic_tokens(bw, tokens, tb, te, is_last);
        else
            encode_fixed_tokens(bw, tokens, tb, te, is_last);
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

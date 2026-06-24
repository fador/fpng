#include "compress/deflate/lz77.hpp"
#include "compress/deflate/match_finder.hpp"

#include <cstring>
#include <algorithm>

namespace fpng {

MatchFinder::MatchFinder() {
    heads1_.resize(TOTAL_HEADS);
    heads2_.resize(TOTAL_HEADS);
    std::fill(heads1_.begin(), heads1_.end(), -1);
    std::fill(heads2_.begin(), heads2_.end(), -1);
}

void MatchFinder::init(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;

    if (prev1_.size() < size) {
        prev1_.resize(std::max(size, size_t(65536)));
        prev2_.resize(std::max(size, size_t(65536)));
    }

    std::fill(heads1_.begin(), heads1_.end(), -1);
    std::fill(heads2_.begin(), heads2_.end(), -1);

    for (size_t i = 0; i + 3 < size; ++i) {
        uint32_t h = hash4(data + i);
        int ss = subslot_offset(data + i);
        size_t idx = head_index(h, ss);
        prev1_[i] = heads1_[idx];
        heads1_[idx] = static_cast<int32_t>(i);
    }
    for (size_t i = 0; i + 4 < size; ++i) {
        uint32_t h = hash_off(data + i);
        int ss = subslot_offset(data + i + 1); // subslot on bytes 2-4
        size_t idx = head_index(h, ss);
        prev2_[i] = heads2_[idx];
        heads2_[idx] = static_cast<int32_t>(i);
    }
}

LZMatch MatchFinder::find_longest(size_t pos, int /*min_len*/) const {
    if (pos + 4 > size_) return {0, 0};

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    LZMatch best{0, 0};

    // Explicit between-row match check
    if (row_stride > 0 && pos >= static_cast<size_t>(row_stride)) {
        size_t row_pos = pos - static_cast<size_t>(row_stride);
        const uint8_t* row_cand = data_ + row_pos;
        uint32_t rc32, cur32;
        std::memcpy(&rc32, row_cand, 4);
        std::memcpy(&cur32, cur, 4);
        if (rc32 == cur32) {
            size_t match_len = simd::match_length(row_cand + 4, cur + 4, max_match - 4) + 4;
            best.length = static_cast<uint16_t>(match_len);
            best.distance = static_cast<uint16_t>(row_stride);
            if (match_len >= static_cast<size_t>(nice_len) || match_len == max_match)
                return best;
        }
    }

    // Helper: walk one chain
    auto walk_chain = [&](const std::vector<int32_t>& heads,
                          const std::vector<int32_t>& prev,
                          uint32_t hash_val, int subslot) {
        size_t idx = head_index(hash_val, subslot);
        int32_t chain_pos = heads[idx];
        size_t chain_len = 0;
        while (chain_pos >= 0 && chain_len < chain_depth) {
            size_t candidate = static_cast<size_t>(chain_pos);
            if (candidate >= pos) { chain_pos = prev[chain_pos]; ++chain_len; continue; }
            if (pos - candidate > limit) break;

            const uint8_t* cand = data_ + candidate;
            uint32_t cand32, cur32;
            std::memcpy(&cand32, cand, 4);
            std::memcpy(&cur32, cur, 4);
            if (cand32 != cur32) {
                chain_pos = prev[chain_pos];
                ++chain_len;
                continue;
            }

            size_t match_len = simd::match_length(cand + 4, cur + 4, max_match - 4) + 4;
            if (match_len > best.length ||
                (match_len == best.length && (pos - candidate) < best.distance)) {
                best.length = static_cast<uint16_t>(match_len);
                best.distance = static_cast<uint16_t>(pos - candidate);
                if (match_len >= static_cast<size_t>(nice_len) || match_len == max_match)
                    break;
            }
            chain_pos = prev[chain_pos];
            ++chain_len;
        }
    };

    // Search both hash chains
    walk_chain(heads1_, prev1_, hash4(cur), subslot_offset(cur));
    if (best.length < static_cast<size_t>(nice_len) && best.length < max_match)
        walk_chain(heads2_, prev2_, hash_off(cur), subslot_offset(cur + 1));

    return best;
}

void MatchFinder::find_all(size_t pos, std::vector<LZMatch>& matches,
                            int /*min_len*/) const {
    matches.clear();
    if (pos + 4 > size_) return;

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    // Helper: walk one chain and collect all matches
    auto walk_chain_all = [&](const std::vector<int32_t>& heads,
                               const std::vector<int32_t>& prev,
                               uint32_t hash_val, int subslot) {
        size_t idx = head_index(hash_val, subslot);
        int32_t chain_pos = heads[idx];
        size_t chain_len = 0;
        while (chain_pos >= 0 && chain_len < chain_depth) {
            size_t candidate = static_cast<size_t>(chain_pos);
            if (candidate >= pos) { chain_pos = prev[chain_pos]; ++chain_len; continue; }
            if (pos - candidate > limit) break;

            const uint8_t* cand = data_ + candidate;
            uint32_t cand32, cur32;
            std::memcpy(&cand32, cand, 4);
            std::memcpy(&cur32, cur, 4);
            if (cand32 != cur32) {
                chain_pos = prev[chain_pos];
                ++chain_len;
                continue;
            }

            size_t match_len = simd::match_length(cand + 4, cur + 4, max_match - 4) + 4;
            if (match_len >= 4) {
                matches.push_back({static_cast<uint16_t>(match_len),
                                   static_cast<uint16_t>(pos - candidate)});
            }
            chain_pos = prev[chain_pos];
            ++chain_len;
        }
    };

    walk_chain_all(heads1_, prev1_, hash4(cur), subslot_offset(cur));
    walk_chain_all(heads2_, prev2_, hash_off(cur), subslot_offset(cur + 1));
}

// LZ77Parser implementation
std::vector<LZ77Parser::Token> LZ77Parser::parse(
    const uint8_t* data, size_t size) {
    return parse(data, size, Options{});
}

std::vector<LZ77Parser::Token> LZ77Parser::parse(
    const uint8_t* data, size_t size, const Options& opts) {

    if (opts.optimal) {
        return parse_optimal(data, size, opts);
    }
    return parse_greedy(data, size, opts);
}

std::vector<LZ77Parser::Token> LZ77Parser::parse_greedy(
    const uint8_t* data, size_t size, const Options& opts) {

    std::vector<Token> tokens;
    tokens.reserve(size);



    // Original hash chain path
    MatchFinder mf;
    mf.chain_depth = opts.chain_depth;
    mf.nice_len = opts.nice_len;
    mf.row_stride = opts.row_stride;
    mf.init(data, size);

    size_t pos = 0;
    while (pos < size) {
        // === Run-length pre-scan ===
        // Check for consecutive identical bytes at common distances.
        // This avoids hash chain overhead for solid-color regions (common in PNGs).
        // Distances: 1=same byte, 3=RGB filtered, 4=RGBA filtered.
        LZMatch rl_match{0, 0};

        // Check distance=1 (identical consecutive bytes)
        if (pos + 4 <= size && data[pos] == data[pos + 1] &&
            data[pos] == data[pos + 2] && data[pos] == data[pos + 3]) {
            size_t run = 4;
            while (pos + run < size && run < static_cast<size_t>(deflate::MAX_MATCH_LEN) &&
                   data[pos + run] == data[pos]) ++run;
            rl_match.length = static_cast<uint16_t>(std::min(run, static_cast<size_t>(deflate::MAX_MATCH_LEN)));
            rl_match.distance = 1;
        }

        // Check distance=3 (RGB filtered: Sub-filter produces 3-byte patterns)
        if (rl_match.length == 0 && pos + 7 <= size &&
            data[pos] == data[pos + 3] && data[pos + 1] == data[pos + 4] &&
            data[pos + 2] == data[pos + 5] && data[pos] == data[pos + 6]) {
            size_t run = 3;
            while (pos + run + 3 <= size &&
                   run + 3 <= static_cast<size_t>(deflate::MAX_MATCH_LEN) &&
                   data[pos + run] == data[pos + run + 3]) ++run;
            rl_match.length = static_cast<uint16_t>(std::min(run + 3, static_cast<size_t>(deflate::MAX_MATCH_LEN)));
            rl_match.distance = 3;
        }

        // Check distance=4 (RGBA filtered: Sub-filter produces 4-byte patterns)
        if (rl_match.length == 0 && pos + 8 <= size &&
            data[pos] == data[pos + 4] && data[pos + 1] == data[pos + 5] &&
            data[pos + 2] == data[pos + 6] && data[pos] == data[pos + 8]) {
            size_t run = 4;
            while (pos + run + 4 <= size &&
                   run + 4 <= static_cast<size_t>(deflate::MAX_MATCH_LEN) &&
                   data[pos + run] == data[pos + run + 4]) ++run;
            rl_match.length = static_cast<uint16_t>(std::min(run + 4, static_cast<size_t>(deflate::MAX_MATCH_LEN)));
            rl_match.distance = 4;
        }

        LZMatch match;
        if (rl_match.length >= static_cast<uint16_t>(opts.min_match)) {
            match = rl_match;
        } else {
            match = mf.find_longest(pos, opts.min_match);
        }

        if (opts.lazy_matching && match.length >= opts.min_match &&
            pos + 1 < size) {
            // Multi-step lazy matching: check pos+1 through pos+lazy_depth
            // for a better deferred match. If one is found, emit literals
            // for the skipped positions and use the deferred match instead.
            int lazy_steps = std::min(opts.lazy_depth, 3);
            LZMatch best_deferred = match;
            int best_skip = 0;

            for (int skip = 1; skip <= lazy_steps && pos + skip < size; ++skip) {
                auto cand = mf.find_longest(pos + static_cast<size_t>(skip), opts.min_match);
                if (cand.length > best_deferred.length + static_cast<uint16_t>(skip)) {
                    best_deferred = cand;
                    best_skip = skip;
                }
            }

            if (best_skip > 0) {
                for (int s = 0; s < best_skip; ++s) {
                    tokens.push_back({Token::LITERAL, data[pos], 0, 0});
                    ++pos;
                }
                match = best_deferred;
                if (match.length >= static_cast<uint16_t>(opts.min_match)) {
                    tokens.push_back({Token::MATCH, 0, match.length, match.distance});
                    pos += match.length;
                    continue;
                }
                continue;
            }
        }

        if (match.length >= static_cast<uint16_t>(opts.min_match)) {
            tokens.push_back({Token::MATCH, 0, match.length, match.distance});
            pos += match.length;
        } else {
            tokens.push_back({Token::LITERAL, data[pos], 0, 0});
            ++pos;
        }
    }

    // Finalize: emit end-of-block
            tokens.push_back({Token::LITERAL, 0, 0, 0}); // EOB marker
    return tokens;
}

std::vector<LZ77Parser::Token> LZ77Parser::parse_optimal(
    const uint8_t* data, size_t size, const Options& opts) {

    constexpr uint64_t INF = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> cost(size + 1, INF);
    std::vector<int> prev_match_len(size + 1, 0);
    std::vector<int> prev_match_dist(size + 1, 0);
    std::vector<bool> is_literal(size + 1, false);
    cost[0] = 0;



    // Hash chain path
    MatchFinder mf;
    mf.chain_depth = opts.chain_depth;
    mf.nice_len = opts.nice_len;
    mf.row_stride = opts.row_stride;
    mf.init(data, size);

    std::vector<LZMatch> matches;
    matches.reserve(128);

    for (size_t i = 0; i < size; ++i) {
        if (cost[i] == INF) continue;

        // Option 1: emit literal
        if (i + 1 <= size) {
            uint64_t c = cost[i] + opts.cost_model.literal_cost(data[i]);
            if (c < cost[i + 1]) {
                cost[i + 1] = c;
                is_literal[i + 1] = true;
            }
        }

        // Option 2: emit match
        mf.find_all(i, matches, opts.min_match);
        for (auto& m : matches) {
            size_t end = i + m.length;
            if (end > size) end = size;
            uint64_t c = cost[i] + opts.cost_model.match_cost(m.length, m.distance);
            if (c < cost[end]) {
                cost[end] = c;
                prev_match_len[end] = m.length;
                prev_match_dist[end] = m.distance;
                is_literal[end] = false;
            }
        }
    }

    // Backtrack to build token list
    std::vector<Token> tokens;
    tokens.reserve(size);
    size_t pos = size;

    while (pos > 0) {
        if (is_literal[pos]) {
            --pos;
            tokens.push_back({Token::LITERAL, data[pos], 0, 0});
        } else {
            uint16_t len = static_cast<uint16_t>(prev_match_len[pos]);
            uint16_t dist = static_cast<uint16_t>(prev_match_dist[pos]);
            pos -= len;
            tokens.push_back({Token::MATCH, 0, len, dist});
        }
    }

    std::reverse(tokens.begin(), tokens.end());
    // Add EOB sentinel (required by write_fixed_block / write_dynamic_block)
    tokens.push_back({Token::LITERAL, 0, 0, 0});
    return tokens;
}

} // namespace fpng

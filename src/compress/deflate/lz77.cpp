#include "compress/deflate/lz77.hpp"
#include "compress/deflate/match_finder.hpp"

#include <cstring>
#include <algorithm>

namespace fpng {

void MatchFinder::init(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;

    // Build sorted array of (4-byte-tag, position) pairs.
    sorted_.clear();
    sorted_.reserve(size);
    for (size_t i = 0; i + 4 <= size; ++i) {
        uint32_t tag;
        std::memcpy(&tag, data + i, 4);
        sorted_.push_back({tag, static_cast<int32_t>(i)});
    }
    std::sort(sorted_.begin(), sorted_.end(),
              [](const TagEntry& a, const TagEntry& b) {
                  if (a.tag != b.tag) return a.tag < b.tag;
                  // Within an equal-tag group, order by descending position so
                  // that the forward scan visits the nearest (most recent)
                  // candidates first. This makes the chain_depth budget and
                  // the nice_len early-exit focus on matches that cost fewer
                  // distance bits.
                  return a.pos > b.pos;
              });

    // Build quick-lookup index: first_[tag >> 16] = first occurrence in sorted_
    first_.assign(INDEX_SIZE + 1, -1);
    for (size_t i = 0; i < sorted_.size(); ++i) {
        uint32_t hi = sorted_[i].tag >> 16;
        if (first_[hi] < 0) first_[hi] = static_cast<int32_t>(i);
    }
    // Fill gaps: first_[k] = first entry with hi >= k
    int32_t last = static_cast<int32_t>(sorted_.size());
    for (int k = INDEX_SIZE - 1; k >= 0; --k) {
        if (first_[k] < 0) first_[k] = last;
        else last = first_[k];
    }
    first_[INDEX_SIZE] = static_cast<int32_t>(sorted_.size());
}

LZMatch MatchFinder::find_longest(size_t pos, int /*min_len*/) const {
    if (pos + 4 > size_) return {0, 0};

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    uint32_t tag;
    std::memcpy(&tag, cur, 4);

    LZMatch best{0, 0};

    // Quick-lookup: narrow search range using high word of tag
    uint32_t hi = tag >> 16;
    int32_t lo = first_[hi];
    int32_t hi_end = first_[hi + 1];
    int32_t found = lo;

    // Binary search for first occurrence of 'tag' within [lo, hi_end)
    while (lo < hi_end) {
        int32_t mid = lo + (hi_end - lo) / 2;
        if (sorted_[mid].tag < tag) lo = mid + 1;
        else hi_end = mid;
    }
    found = lo;

    // Scan forward through all entries with matching tag
    size_t scanned = 0;
    while (found < static_cast<int32_t>(sorted_.size()) &&
           sorted_[found].tag == tag && scanned < chain_depth) {
        size_t candidate = static_cast<size_t>(sorted_[found].pos);
        ++found;
        if (candidate >= pos) continue;
        size_t dist = pos - candidate;
        if (dist > limit) continue;

        size_t match_len = simd::match_length(data_ + candidate + 4, cur + 4,
                                               max_match - 4) + 4;
        // Cost-aware selection
        int dc = deflate::distance_code(static_cast<uint16_t>(dist));
        int extra = deflate::distance_extra_bits(dc);
        int new_score = static_cast<int>(match_len) * 256 - extra * 32;
        int bdc = deflate::distance_code(best.distance);
        int bextra = deflate::distance_extra_bits(bdc);
        int best_score = static_cast<int>(best.length) * 256 - bextra * 32;

        if (new_score > best_score ||
            (new_score == best_score && dist < best.distance)) {
            best.length = static_cast<uint16_t>(match_len);
            best.distance = static_cast<uint16_t>(dist);
            if (match_len >= max_match || match_len >= static_cast<size_t>(nice_len))
                break;
        }
        ++scanned;
    }

    // Row-stride probe: the filter byte at row boundaries means the 4-byte
    // tag at pos and (pos - row_stride) may differ. Explicitly check this
    // distance — common for between-row matches in filtered PNG data.
    if (row_stride > 0 && pos >= static_cast<size_t>(row_stride) &&
        best.length < static_cast<size_t>(nice_len)) {
        size_t row_pos = pos - static_cast<size_t>(row_stride);
        size_t mlen = simd::match_length(data_ + row_pos, cur, max_match);
        if (mlen >= 4) {
            int dc = deflate::distance_code(static_cast<uint16_t>(row_stride));
            int extra = deflate::distance_extra_bits(dc);
            int new_score = static_cast<int>(mlen) * 256 - extra * 32;
            int bdc = deflate::distance_code(best.distance);
            int bextra = deflate::distance_extra_bits(bdc);
            int best_score = static_cast<int>(best.length) * 256 - bextra * 32;
            if (new_score > best_score) {
                best.length = static_cast<uint16_t>(mlen);
                best.distance = static_cast<uint16_t>(row_stride);
            }
        }
    }

    return best;
}

void MatchFinder::find_all(size_t pos, std::vector<LZMatch>& matches,
                            int /*min_len*/) const {
    matches.clear();
    if (pos + 4 > size_) return;

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    uint32_t tag;
    std::memcpy(&tag, cur, 4);

    // Binary search for matching tag
    uint32_t hi = tag >> 16;
    int32_t lo = first_[hi];
    int32_t hi_end = first_[hi + 1];
    while (lo < hi_end) {
        int32_t mid = lo + (hi_end - lo) / 2;
        if (sorted_[mid].tag < tag) lo = mid + 1;
        else hi_end = mid;
    }

    // Collect all matching positions
    size_t scanned = 0;
    while (lo < static_cast<int32_t>(sorted_.size()) &&
           sorted_[lo].tag == tag && scanned < chain_depth * 2) {
        size_t candidate = static_cast<size_t>(sorted_[lo].pos);
        ++lo;
        if (candidate >= pos) continue;
        size_t dist = pos - candidate;
        if (dist > limit) continue;

        size_t match_len = simd::match_length(data_ + candidate + 4, cur + 4,
                                               max_match - 4) + 4;
        if (match_len >= 4) {
            matches.push_back({static_cast<uint16_t>(match_len),
                               static_cast<uint16_t>(dist)});
        }
        ++scanned;
    }
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

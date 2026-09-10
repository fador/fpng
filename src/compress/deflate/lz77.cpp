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

    // Within the equal-tag group positions are ordered descending, so all
    // entries with pos >= current pos precede the usable candidates. Binary
    // search that boundary instead of linear-scanning past every future
    // position (which made this O(N) per lookup / O(N^2) overall on a
    // full-stream index).
    {
        int32_t a = found, b = static_cast<int32_t>(sorted_.size());
        while (a < b) {
            int32_t mid = a + (b - a) / 2;
            if (sorted_[mid].tag <= tag) a = mid + 1;
            else b = mid;
        }
        int32_t tag_end = a;
        a = found; b = tag_end;
        while (a < b) {
            int32_t mid = a + (b - a) / 2;
            if (sorted_[mid].pos >= static_cast<int32_t>(pos)) a = mid + 1;
            else b = mid;
        }
        found = a;
    }

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

    // Skip entries with pos >= current pos (see find_longest).
    {
        int32_t a = lo, b = static_cast<int32_t>(sorted_.size());
        while (a < b) {
            int32_t mid = a + (b - a) / 2;
            if (sorted_[mid].tag <= tag) a = mid + 1;
            else b = mid;
        }
        int32_t tag_end = a;
        a = lo; b = tag_end;
        while (a < b) {
            int32_t mid = a + (b - a) / 2;
            if (sorted_[mid].pos >= static_cast<int32_t>(pos)) a = mid + 1;
            else b = mid;
        }
        lo = a;
    }

    // Collect all matching positions. Candidates are visited nearest-first
    // (distance increasing), so keep only matches that set a new longest
    // length. Shorter-but-farther matches are dominated and would only bloat
    // the optimal-parser DP. This caps the returned list at ~MAX_MATCH_LEN.
    size_t scanned = 0;
    size_t best_len = 0;
    while (lo < static_cast<int32_t>(sorted_.size()) &&
           sorted_[lo].tag == tag && scanned < chain_depth * 2) {
        size_t candidate = static_cast<size_t>(sorted_[lo].pos);
        ++lo;
        if (candidate >= pos) continue;
        size_t dist = pos - candidate;
        if (dist > limit) continue;

        size_t match_len = simd::match_length(data_ + candidate + 4, cur + 4,
                                               max_match - 4) + 4;
        if (match_len >= 4 && match_len > best_len) {
            best_len = match_len;
            matches.push_back({static_cast<uint16_t>(match_len),
                               static_cast<uint16_t>(dist)});
            if (match_len >= max_match) break;
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
    return parse_range(data, size, 0, size, opts, nullptr);
}

std::vector<LZ77Parser::Token> LZ77Parser::parse_range(
    const uint8_t* data, size_t data_size, size_t start, size_t end,
    const Options& opts, const MatchFinder* shared_mf) {
    if (end > data_size) end = data_size;
    if (start > end) start = end;
    if (opts.optimal)
        return parse_optimal(data, data_size, start, end, opts, shared_mf);
    return parse_greedy(data, data_size, start, end, opts, shared_mf);
}

std::vector<LZ77Parser::Token> LZ77Parser::parse_greedy(
    const uint8_t* data, size_t data_size, size_t start, size_t end,
    const Options& opts, const MatchFinder* shared_mf) {

    std::vector<Token> tokens;
    tokens.reserve(end - start + 1);

    MatchFinder local_mf;
    const MatchFinder* mf = shared_mf;
    if (!mf) {
        local_mf.chain_depth = opts.chain_depth;
        local_mf.nice_len = opts.nice_len;
        local_mf.row_stride = opts.row_stride;
        local_mf.init(data, data_size);
        mf = &local_mf;
    }

    size_t pos = start;
    while (pos < end) {
        // === Run-length pre-scan ===
        // Check for periodic patterns at common distances. A match at
        // distance D starting at `pos` copies bytes from `pos - D`, so the
        // seed bytes must also match: data[pos-D+i] == data[pos+i] for
        // i in [0, D). Verifying only the forward repeats is insufficient
        // (the byte before the run typically differs from the run value).
        // Distances: 1=same byte, 3=RGB filtered, 4=RGBA filtered.
        LZMatch rl_match{0, 0};
        const size_t RL_MAX = static_cast<size_t>(deflate::MAX_MATCH_LEN);

        // Check distance=1 (identical consecutive bytes)
        if (pos >= 1 && data[pos - 1] == data[pos]) {
            size_t run = 1;
            while (pos + run < end && run < RL_MAX &&
                   data[pos + run] == data[pos]) ++run;
            rl_match.length = static_cast<uint16_t>(run);
            rl_match.distance = 1;
        }

        // Check distance=3 (RGB filtered: Sub-filter produces 3-byte patterns)
        if (rl_match.length == 0 && pos >= 3 && pos + 3 <= end &&
            data[pos - 3] == data[pos] &&
            data[pos - 2] == data[pos + 1] &&
            data[pos - 1] == data[pos + 2]) {
            size_t run = 3;
            while (pos + run < end && run < RL_MAX &&
                   data[pos + run] == data[pos + run - 3]) ++run;
            rl_match.length = static_cast<uint16_t>(run);
            rl_match.distance = 3;
        }

        // Check distance=4 (RGBA filtered: Sub-filter produces 4-byte patterns)
        if (rl_match.length == 0 && pos >= 4 && pos + 4 <= end &&
            data[pos - 4] == data[pos] &&
            data[pos - 3] == data[pos + 1] &&
            data[pos - 2] == data[pos + 2] &&
            data[pos - 1] == data[pos + 3]) {
            size_t run = 4;
            while (pos + run < end && run < RL_MAX &&
                   data[pos + run] == data[pos + run - 4]) ++run;
            rl_match.length = static_cast<uint16_t>(run);
            rl_match.distance = 4;
        }

        LZMatch match;
        if (rl_match.length >= static_cast<uint16_t>(opts.min_match)) {
            match = rl_match;
        } else {
            match = mf->find_longest(pos, opts.min_match);
        }
        // A match may not extend beyond the end of this block.
        if (match.length > end - pos)
            match.length = static_cast<uint16_t>(end - pos);

        if (opts.lazy_matching && match.length >= opts.min_match &&
            pos + 1 < end) {
            // Multi-step lazy matching: check pos+1 through pos+lazy_depth
            // for a better deferred match. If one is found, emit literals
            // for the skipped positions and use the deferred match instead.
            int lazy_steps = std::min(opts.lazy_depth, 3);
            LZMatch best_deferred = match;
            int best_skip = 0;

            for (int skip = 1; skip <= lazy_steps && pos + skip < end; ++skip) {
                auto cand = mf->find_longest(pos + static_cast<size_t>(skip),
                                             opts.min_match);
                if (cand.length > end - (pos + skip))
                    cand.length = static_cast<uint16_t>(end - (pos + skip));
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
    const uint8_t* data, size_t data_size, size_t start, size_t end,
    const Options& opts, const MatchFinder* shared_mf) {

    constexpr uint64_t INF = std::numeric_limits<uint64_t>::max();
    const size_t len = end - start;

    // Reuse buffers across iterations (they only grow).
    auto& cost = ws_.cost;
    auto& prev_match_len = ws_.prev_match_len;
    auto& prev_match_dist = ws_.prev_match_dist;
    auto& is_literal = ws_.is_literal;
    cost.assign(len + 1, INF);
    prev_match_len.assign(len + 1, 0);
    prev_match_dist.assign(len + 1, 0);
    is_literal.assign(len + 1, false);
    cost[0] = 0;

    MatchFinder local_mf;
    const MatchFinder* mf = shared_mf;
    if (!mf) {
        local_mf.chain_depth = opts.chain_depth;
        local_mf.nice_len = opts.nice_len;
        local_mf.row_stride = opts.row_stride;
        local_mf.init(data, data_size);
        mf = &local_mf;
    }

    auto& matches = ws_.matches;
    matches.reserve(128);

    for (size_t li = 0; li < len; ++li) {
        if (cost[li] == INF) continue;
        size_t i = start + li;

        // Option 1: emit literal
        {
            uint64_t c = cost[li] + opts.cost_model.literal_cost(data[i]);
            if (c < cost[li + 1]) {
                cost[li + 1] = c;
                is_literal[li + 1] = true;
            }
        }

        // Option 2: emit match (clamped to the block end)
        mf->find_all(i, matches, opts.min_match);
        for (auto& m : matches) {
            size_t mlen = m.length;
            if (mlen > end - i) mlen = end - i;
            if (mlen < static_cast<size_t>(opts.min_match)) continue;
            size_t nli = li + mlen;
            uint64_t c = cost[li] +
                         opts.cost_model.match_cost(static_cast<uint16_t>(mlen),
                                                    m.distance);
            if (c < cost[nli]) {
                cost[nli] = c;
                prev_match_len[nli] = static_cast<int>(mlen);
                prev_match_dist[nli] = m.distance;
                is_literal[nli] = false;
            }
        }
    }

    // Backtrack to build token list
    std::vector<Token> tokens;
    tokens.reserve(len + 1);
    size_t pos = len;

    while (pos > 0) {
        if (is_literal[pos]) {
            --pos;
            tokens.push_back({Token::LITERAL, data[start + pos], 0, 0});
        } else {
            uint16_t l = static_cast<uint16_t>(prev_match_len[pos]);
            uint16_t d = static_cast<uint16_t>(prev_match_dist[pos]);
            pos -= l;
            tokens.push_back({Token::MATCH, 0, l, d});
        }
    }

    std::reverse(tokens.begin(), tokens.end());
    // Add EOB sentinel (required by the block writers)
    tokens.push_back({Token::LITERAL, 0, 0, 0});
    return tokens;
}

} // namespace fpng

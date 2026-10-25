#include "compress/deflate/lz77.hpp"
#include "compress/deflate/match_finder.hpp"

#include <cstring>
#include <algorithm>

namespace fpng {

void MatchFinder::init(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;

    // Build sorted array of (4-byte-tag, position) pairs, ordered by tag
    // ascending and, within an equal-tag group, by descending position so
    // that the forward scan visits the nearest (most recent) candidates
    // first. (tag, pos) pairs are unique, so this order is fully determined;
    // an LSD radix sort on the tag, fed in descending-position order, yields
    // exactly the same array as a comparison sort in O(n) instead of
    // O(n log n).
    const size_t n = size >= 4 ? size - 3 : 0;
    sorted_.resize(n);
    {
        std::vector<TagEntry> tmp(n);
        uint32_t hist[4][256] = {};
        for (size_t k = 0; k < n; ++k) {
            size_t i = n - 1 - k; // descending position
            uint32_t tag;
            std::memcpy(&tag, data + i, 4);
            tmp[k] = {tag, static_cast<int32_t>(i)};
            hist[0][tag & 0xFF]++;
            hist[1][(tag >> 8) & 0xFF]++;
            hist[2][(tag >> 16) & 0xFF]++;
            hist[3][tag >> 24]++;
        }
        TagEntry* src = tmp.data();
        TagEntry* dst = sorted_.data();
        for (int pass = 0; pass < 4; ++pass) {
            const int shift = pass * 8;
            if (n == 0 || hist[pass][(src[0].tag >> shift) & 0xFF] == n)
                continue; // all keys share this digit: pass is the identity
            uint32_t ofs[256];
            uint32_t sum = 0;
            for (int b = 0; b < 256; ++b) { ofs[b] = sum; sum += hist[pass][b]; }
            for (size_t k = 0; k < n; ++k) {
                const TagEntry& e = src[k];
                dst[ofs[(e.tag >> shift) & 0xFF]++] = e;
            }
            std::swap(src, dst);
        }
        if (src != sorted_.data())
            std::memcpy(sorted_.data(), src, n * sizeof(TagEntry));
    }

    // Build quick-lookup index: first_[k] = first entry with (tag >> 16) >= k.
    // Walk the distinct high words once and block-fill the gaps (vectorizable)
    // instead of a 64K-step scalar loop; matters for small trial buffers.
    first_.resize(INDEX_SIZE + 1);
    {
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            size_t hi = sorted_[i].tag >> 16;
            if (hi >= k) {
                std::fill(first_.begin() + k, first_.begin() + hi + 1, static_cast<int32_t>(i));
                k = hi + 1;
            }
        }
        std::fill(first_.begin() + k, first_.end(), static_cast<int32_t>(n));
    }

    // Build 3-byte "nearest previous occurrence" chain for length-3 matches.
    prev3_.assign(size + 1, -1);
    size_t hbits = 12;
    while ((size_t(1) << hbits) < size * 2 && hbits < 21) ++hbits;
    size_t hsize = size_t(1) << hbits;
    hash3_table_.assign(hsize, -1);
    for (size_t i = 0; i + 3 <= size; ++i) {
        uint32_t key = (uint32_t(data[i]) << 16) |
                       (uint32_t(data[i + 1]) << 8) | data[i + 2];
        uint32_t h = (key * 2654435761u) >> (32 - hbits);
        prev3_[i] = hash3_table_[h];
        hash3_table_[h] = static_cast<int32_t>(i);
    }
}

LZMatch MatchFinder::find_longest(size_t pos, int /*min_len*/) const {
    if (pos + 3 > size_) return {0, 0};

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    auto score = [](size_t len, size_t dist) -> int {
        int dc = deflate::distance_code(static_cast<uint16_t>(dist));
        int extra = (dc >= 0) ? deflate::distance_extra_bits(dc) : 0;
        return static_cast<int>(len) * 256 - extra * 32;
    };

    LZMatch best{0, 0};

    // Length-3 candidate from the 3-byte index. Walk chain up to 64 steps
    // to find nearest valid 3-byte match.
    if (pos + 3 <= size_) {
        int32_t p3 = prev3_[pos];
        int steps = 0;
        while (p3 >= 0 && steps < 64) {
            size_t cand = static_cast<size_t>(p3);
            if (cand >= pos) break;
            size_t dist = pos - cand;
            if (dist > limit) break;
            if (data_[cand] == data_[pos] &&
                data_[cand + 1] == data_[pos + 1] &&
                data_[cand + 2] == data_[pos + 2]) {
                best.length = deflate::MIN_MATCH_LEN;
                best.distance = static_cast<uint16_t>(dist);
                break;
            }
            p3 = prev3_[cand];
            ++steps;
        }
    }

    if (pos + 4 <= size_) {
        uint32_t tag;
        std::memcpy(&tag, cur, 4);

        // Quick-lookup: narrow search range using high word of tag
        uint32_t hi = tag >> 16;
        int32_t lo = first_[hi];
        int32_t hi_end = first_[hi + 1];

        // Binary search for first occurrence of 'tag' within [lo, hi_end)
        while (lo < hi_end) {
            int32_t mid = lo + (hi_end - lo) / 2;
            if (sorted_[mid].tag < tag) lo = mid + 1;
            else hi_end = mid;
        }

        // Only search if the tag is actually present
        if (lo < first_[hi + 1] && sorted_[lo].tag == tag) {
            int32_t found = lo;
            int32_t range_end = first_[hi + 1];

            // Binary search for upper bound of equal-tag group
            int32_t a = found, b = range_end;
            while (a < b) {
                int32_t mid = a + (b - a) / 2;
                if (sorted_[mid].tag <= tag) a = mid + 1;
                else b = mid;
            }
            int32_t tag_end = a;

            // Equal-tag positions are descending. Find first with pos < current pos
            a = found; b = tag_end;
            while (a < b) {
                int32_t mid = a + (b - a) / 2;
                if (sorted_[mid].pos >= static_cast<int32_t>(pos)) a = mid + 1;
                else b = mid;
            }
            found = a;

            size_t scanned = 0;
            while (found < tag_end && scanned < chain_depth) {
                size_t candidate = static_cast<size_t>(sorted_[found].pos);
                ++found;
                size_t dist = pos - candidate;
                if (dist > limit) break; // descending pos -> strictly increasing dist

                // Inside this loop best.distance <= dist (the 3-byte chain
                // returns the nearest 3-byte match and tag candidates come in
                // increasing distance), and extra bits are non-decreasing in
                // distance, so a candidate can only win with a strictly longer
                // match, which requires a match at offset best.length.
                if (best.length >= 4 && best.length < max_match &&
                    data_[candidate + best.length] != cur[best.length]) {
                    ++scanned;
                    continue;
                }
                size_t match_len = simd::match_length(data_ + candidate + 4, cur + 4,
                                                       max_match - 4) + 4;
                int new_score = score(match_len, dist);
                int best_score = score(best.length, best.distance);

                if (new_score > best_score ||
                    (new_score == best_score && dist < best.distance)) {
                    best.length = static_cast<uint16_t>(match_len);
                    best.distance = static_cast<uint16_t>(dist);
                    if (match_len >= max_match || match_len >= static_cast<size_t>(nice_len))
                        break;
                }
                ++scanned;
            }
        }
    }

    // Row-stride and diagonal 2D spatial probes (1x and 2x row_stride)
    if (row_stride > 0 && best.length < static_cast<size_t>(nice_len)) {
        int d_offsets[] = {0, -4, 4, -3, 3, -2, 2, -1, 1};
        for (int mult = 1; mult <= 2; ++mult) {
            int base_stride = row_stride * mult;
            for (int doff : d_offsets) {
                int d = base_stride + doff;
                if (d > 0 && pos >= static_cast<size_t>(d) && static_cast<size_t>(d) <= limit) {
                    size_t row_pos = pos - static_cast<size_t>(d);
                    size_t mlen = simd::match_length(data_ + row_pos, cur, max_match);
                    if (mlen >= static_cast<size_t>(deflate::MIN_MATCH_LEN)) {
                        int new_score = score(mlen, d);
                        int best_score = score(best.length, best.distance);
                        if (new_score > best_score ||
                            (new_score == best_score && static_cast<uint16_t>(d) < best.distance)) {
                            best.length = static_cast<uint16_t>(mlen);
                            best.distance = static_cast<uint16_t>(d);
                        }
                    }
                }
            }
        }
    }

    return best;
}

void MatchFinder::find_all(size_t pos, std::vector<LZMatch>& matches,
                            int /*min_len*/) const {
    matches.clear();
    if (pos + 3 > size_) return;

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    // For every length L the parser needs the minimum distance of any probe
    // whose match length is >= L. Instead of filling [3..len] for each probe
    // (O(258) per probe), record the minimum distance per exact length and
    // take a suffix minimum once at the end. The result is identical.
    constexpr uint16_t NONE = 0xFFFF; // distances are <= 32768
    uint16_t best_d[deflate::MAX_MATCH_LEN + 1];
    std::fill(std::begin(best_d), std::end(best_d), NONE);
    size_t max_found_len = 0;
    bool have_dist1 = false;

    auto record_match = [&](size_t mlen, uint16_t dist) {
        if (mlen > max_match) mlen = max_match;
        if (mlen < static_cast<size_t>(deflate::MIN_MATCH_LEN)) return;
        if (dist < best_d[mlen]) best_d[mlen] = dist;
        if (mlen > max_found_len) max_found_len = mlen;
    };

    // 1. Run-length / periodic check for distances 1, 2, 3, 4
    for (size_t d = 1; d <= 4; ++d) {
        if (pos >= d && d <= limit && pos + 3 <= size_) {
            if (data_[pos - d] == cur[0] &&
                data_[pos - d + 1] == cur[1] &&
                data_[pos - d + 2] == cur[2]) {
                size_t run = 3 + simd::match_length(data_ + pos - d + 3, cur + 3, max_match - 3);
                record_match(run, static_cast<uint16_t>(d));
                if (d == 1) have_dist1 = true;
                if (d == 1 && run >= max_match) break;
            }
        }
    }

    // 2. Length-3 nearest match from the 3-byte index. Walk chain up to 64 steps.
    if (!have_dist1) {
        int32_t p3 = prev3_[pos];
        int steps = 0;
        while (p3 >= 0 && steps < 64) {
            size_t cand = static_cast<size_t>(p3);
            if (cand >= pos) break;
            size_t dist = pos - cand;
            if (dist > limit) break;
            if (data_[cand] == cur[0] &&
                data_[cand + 1] == cur[1] &&
                data_[cand + 2] == cur[2]) {
                record_match(deflate::MIN_MATCH_LEN, static_cast<uint16_t>(dist));
                break;
            }
            p3 = prev3_[cand];
            ++steps;
        }
    }

    // 3. 4-byte tag matches (sorted descending by pos -> strictly increasing dist)
    if (pos + 4 <= size_ && max_found_len < max_match) {
        uint32_t tag;
        std::memcpy(&tag, cur, 4);

        uint32_t hi = tag >> 16;
        int32_t lo = first_[hi];
        int32_t hi_end = first_[hi + 1];
        while (lo < hi_end) {
            int32_t mid = lo + (hi_end - lo) / 2;
            if (sorted_[mid].tag < tag) lo = mid + 1;
            else hi_end = mid;
        }

        if (lo < first_[hi + 1] && sorted_[lo].tag == tag) {
            int32_t found = lo;
            int32_t range_end = first_[hi + 1];

            int32_t a = found, b = range_end;
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

            size_t scanned = 0;
            size_t best_tag_len = 3;
            const size_t scan_limit = chain_depth * 2;
            while (found < tag_end && scanned < scan_limit) {
                size_t candidate = static_cast<size_t>(sorted_[found].pos);
                ++found;
                size_t dist = pos - candidate;
                if (dist > limit) break; // descending pos -> strictly increasing dist

                // A candidate can only improve on best_tag_len if it also
                // matches at offset best_tag_len (< max_match, in bounds).
                // Candidates failing this check could not change the result.
                const uint8_t* cp = data_ + candidate;
                if (cp[best_tag_len] == cur[best_tag_len]) {
                    size_t match_len = simd::match_length(cp + 4, cur + 4,
                                                           max_match - 4) + 4;
                    if (match_len > best_tag_len) {
                        // Equivalent to the old per-length update: every
                        // shorter length already has an earlier (closer)
                        // candidate recorded.
                        record_match(match_len, static_cast<uint16_t>(dist));
                        best_tag_len = match_len;
                        if (match_len >= max_match) break;
                    }
                }
                ++scanned;
            }
        }
    }

    // 4. Row-stride and diagonal 2D spatial probes (1x and 2x row_stride)
    if (row_stride > 0) {
        int d_offsets[] = {0, -4, 4, -3, 3, -2, 2, -1, 1};
        for (int mult = 1; mult <= 2; ++mult) {
            int base_stride = row_stride * mult;
            for (int doff : d_offsets) {
                int d = base_stride + doff;
                if (d > 0 && pos >= static_cast<size_t>(d) && static_cast<size_t>(d) <= limit) {
                    size_t row_pos = pos - static_cast<size_t>(d);
                    size_t mlen = simd::match_length(data_ + row_pos, cur, max_match);
                    if (mlen >= static_cast<size_t>(deflate::MIN_MATCH_LEN)) {
                        record_match(mlen, static_cast<uint16_t>(d));
                    }
                }
            }
        }
    }

    // Emit all achievable lengths [3..max_found_len] with their minimum
    // distances (suffix minimum over the per-length bests).
    if (max_found_len < static_cast<size_t>(deflate::MIN_MATCH_LEN)) return;
    size_t first = matches.size();
    matches.resize(first + (max_found_len - deflate::MIN_MATCH_LEN + 1));
    uint16_t running = NONE;
    for (size_t L = max_found_len; L >= static_cast<size_t>(deflate::MIN_MATCH_LEN); --L) {
        if (best_d[L] < running) running = best_d[L];
        matches[first + (L - deflate::MIN_MATCH_LEN)] = {static_cast<uint16_t>(L), running};
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

    // Exact cost tables. CostModel::match_cost() is a sum of a length part
    // (symbol + extra bits) and a distance part, so precomputing both per
    // pass gives bit-identical costs without per-candidate code lookups.
    uint64_t lit_tab[256];
    uint64_t len_tab[deflate::MAX_MATCH_LEN + 1] = {};
    uint64_t dist_tab[deflate::MAX_DIST_SYMS] = {};
    {
        const auto& cm = opts.cost_model;
        for (int b = 0; b < 256; ++b) lit_tab[b] = cm.literal_cost(static_cast<uint8_t>(b));
        for (int L = deflate::MIN_MATCH_LEN; L <= deflate::MAX_MATCH_LEN; ++L) {
            int lc = deflate::length_code(L);
            uint64_t ex = static_cast<uint64_t>(deflate::length_extra_bits(lc));
            if (cm.precomputed_costs)
                len_tab[L] = cm.precomputed_costs[257 + lc] + (ex << 10);
            else if (cm.litlen_lengths && cm.dist_lengths)
                len_tab[L] = cm.litlen_lengths[257 + lc] + ex;
            else
                len_tab[L] = 15 + static_cast<uint64_t>(L) / 4;
        }
        for (int dc = 0; dc < 30; ++dc) {
            uint64_t ex = static_cast<uint64_t>(deflate::distance_extra_bits(dc));
            if (cm.precomputed_costs)
                dist_tab[dc] = cm.precomputed_costs[288 + dc] + (ex << 10);
            else if (cm.litlen_lengths && cm.dist_lengths)
                dist_tab[dc] = cm.dist_lengths[dc] + ex;
            else
                dist_tab[dc] = 0;
        }
    }
    const size_t min_match = static_cast<size_t>(opts.min_match);
    const bool cached = mf->has_match_cache();
    uint64_t* const costp = cost.data();
    int* const pml = prev_match_len.data();
    int* const pmd = prev_match_dist.data();
    uint8_t* const isl = is_literal.data();

    // Relax edge li -> li+mlen with cost c (tie-breaking as before).
    auto relax = [&](size_t li, size_t mlen, uint16_t dist, uint64_t c) {
        size_t nli = li + mlen;
        if (c < costp[nli] ||
            (c == costp[nli] &&
             (isl[nli] ||
              static_cast<int>(mlen) > pml[nli] ||
              (static_cast<int>(mlen) == pml[nli] && dist < pmd[nli])))) {
            costp[nli] = c;
            pml[nli] = static_cast<int>(mlen);
            pmd[nli] = dist;
            isl[nli] = 0;
        }
    };

    for (size_t li = 0; li < len; ++li) {
        if (costp[li] == INF) continue;
        size_t i = start + li;
        const uint64_t base = costp[li];

        // Option 1: emit literal
        {
            uint64_t c = base + lit_tab[data[i]];
            if (c < costp[li + 1]) {
                costp[li + 1] = c;
                isl[li + 1] = 1;
            }
        }

        // Option 2: emit match (clamped to the block end)
        const size_t room = end - i;
        if (cached) {
            const uint32_t* sb;
            const uint32_t* se;
            mf->cached_segments(i, sb, se);
            size_t L = deflate::MIN_MATCH_LEN;
            for (; sb != se; ++sb) {
                size_t last = *sb >> 16;
                uint16_t dist = static_cast<uint16_t>(*sb & 0xFFFF);
                uint64_t bd = base + dist_tab[deflate::distance_code(dist)];
                for (; L <= last; ++L) {
                    size_t mlen = L > room ? room : L;
                    if (mlen < min_match) continue;
                    relax(li, mlen, dist, bd + len_tab[mlen]);
                }
            }
        } else {
            mf->find_all(i, matches, opts.min_match);
            for (auto& m : matches) {
                size_t mlen = m.length;
                if (mlen > room) mlen = room;
                if (mlen < min_match) continue;
                relax(li, mlen, m.distance,
                      base + dist_tab[deflate::distance_code(m.distance)] + len_tab[mlen]);
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

bool MatchFinder::build_match_cache(size_t max_entries) {
    cache_off_.clear();
    cache_seg_.clear();
    if (size_ == 0 || size_ >= 0xFFFFFFFFu) return false;
    std::vector<uint32_t> off(size_ + 1, 0);
    std::vector<uint32_t> seg;
    seg.reserve(std::min(max_entries, size_ * 2));
    std::vector<LZMatch> m;
    m.reserve(260);
    for (size_t p = 0; p < size_; ++p) {
        off[p] = static_cast<uint32_t>(seg.size());
        find_all(p, m, deflate::MIN_MATCH_LEN);
        // find_all emits every length 3..max contiguously, ascending, with
        // non-decreasing distance; keep only the last length of each run.
        if (!m.empty() && m[0].length != deflate::MIN_MATCH_LEN) return false;
        for (size_t k = 0; k < m.size(); ++k) {
            if (k + 1 < m.size() && m[k + 1].length != m[k].length + 1) return false;
            if (k + 1 == m.size() || m[k + 1].distance != m[k].distance)
                seg.push_back((static_cast<uint32_t>(m[k].length) << 16) | m[k].distance);
        }
        if (seg.size() > max_entries) return false; // too large; stay uncached
    }
    off[size_] = static_cast<uint32_t>(seg.size());
    cache_off_ = std::move(off);
    cache_seg_ = std::move(seg);
    return true;
}

void MatchFinder::find_all_cached(size_t pos, std::vector<LZMatch>& matches) const {
    matches.clear();
    if (pos >= size_) return;
    uint32_t b = cache_off_[pos], e = cache_off_[pos + 1];
    uint16_t L = deflate::MIN_MATCH_LEN;
    for (uint32_t k = b; k < e; ++k) {
        uint16_t last = static_cast<uint16_t>(cache_seg_[k] >> 16);
        uint16_t dist = static_cast<uint16_t>(cache_seg_[k] & 0xFFFF);
        for (; L <= last; ++L) matches.push_back({L, dist});
    }
}

} // namespace fpng

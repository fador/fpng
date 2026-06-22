#include "compress/deflate/lz77.hpp"

#include <cstring>
#include <algorithm>

namespace fpng {

MatchFinder::MatchFinder() {
    heads_.resize(HASH_SIZE);
    std::fill(heads_.begin(), heads_.end(), -1);
}

void MatchFinder::init(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;

    if (prev_.size() < size) {
        prev_.resize(std::max(size, size_t(65536)));
    }

    std::fill(heads_.begin(), heads_.end(), -1);

    for (size_t i = 0; i + 2 < size; ++i) {
        uint32_t h = hash3(data + i);
        prev_[i] = heads_[h];
        heads_[h] = static_cast<int32_t>(i);
    }
}

LZMatch MatchFinder::find_longest(size_t pos, int min_len) const {
    if (pos + static_cast<size_t>(min_len) > size_) return {0, 0};

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    uint32_t h = hash3(cur);
    int32_t chain_pos = heads_[h];

    LZMatch best{0, 0};
    size_t chain_len = 0;

    while (chain_pos >= 0 && chain_len < MAX_CHAIN) {
        size_t candidate = static_cast<size_t>(chain_pos);
        if (candidate >= pos) { chain_pos = prev_[chain_pos]; ++chain_len; continue; }
        if (pos - candidate > limit) break;

        const uint8_t* cand = data_ + candidate;

        // Quick check on first min_len bytes
        if (cand[0] == cur[0] && cand[1] == cur[1] && cand[2] == cur[2]) {
            size_t match_len = min_len;
            while (match_len < max_match && cand[match_len] == cur[match_len])
                ++match_len;

            if (match_len > best.length) {
                best.length = static_cast<uint16_t>(match_len);
                best.distance = static_cast<uint16_t>(pos - candidate);
                if (match_len == max_match) break;
            }
        }

        chain_pos = prev_[chain_pos];
        ++chain_len;
    }

    return best;
}

void MatchFinder::find_all(size_t pos, std::vector<LZMatch>& matches,
                            int min_len) const {
    matches.clear();
    if (pos + min_len > size_) return;

    const uint8_t* cur = data_ + pos;
    size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
    size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));

    uint32_t h = hash3(cur);
    int32_t chain_pos = heads_[h];

    size_t chain_len = 0;
    while (chain_pos >= 0 && chain_len < MAX_CHAIN) {
        size_t candidate = static_cast<size_t>(chain_pos);
        if (candidate >= pos) { chain_pos = prev_[chain_pos]; ++chain_len; continue; }
        if (pos - candidate > limit) break;

        const uint8_t* cand = data_ + candidate;

        if (cand[0] == cur[0] && cand[1] == cur[1] && cand[2] == cur[2]) {
            size_t match_len = min_len;
            while (match_len < max_match && cand[match_len] == cur[match_len])
                ++match_len;
            if (match_len >= static_cast<size_t>(min_len)) {
                matches.push_back({
                    static_cast<uint16_t>(match_len),
                    static_cast<uint16_t>(pos - candidate)
                });
            }
        }

        chain_pos = prev_[chain_pos];
        ++chain_len;
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

    MatchFinder mf;
    mf.init(data, size);

    std::vector<Token> tokens;
    tokens.reserve(size);

    size_t pos = 0;
    while (pos < size) {
        auto match = mf.find_longest(pos, opts.min_match);

        if (opts.lazy_matching && match.length >= opts.min_match &&
            pos + 1 < size) {
            // Try lazy matching: look ahead
            auto next_match = mf.find_longest(pos + 1, opts.min_match);

            if (next_match.length > match.length + 1) {
                // Emit literal, use next match
                tokens.push_back({Token::LITERAL, data[pos], 0, 0});
                ++pos;
                match = next_match;
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

    MatchFinder mf;
    mf.init(data, size);

    // Forward DP for optimal parsing
    // cost[i] = minimum bits to encode data[0..i)
    constexpr uint64_t INF = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> cost(size + 1, INF);
    std::vector<int> prev_match_len(size + 1, 0);
    std::vector<int> prev_match_dist(size + 1, 0);
    std::vector<bool> is_literal(size + 1, false);

    cost[0] = 0;

    std::vector<LZMatch> matches;
    matches.reserve(128); // MatchFinder::MAX_CHAIN

    // Estimate costs: literals ~8 bits, matches ~(len/4 + 10) bits
    // This is approximate - exact costs require Huffman codes
    auto lit_cost = [](uint8_t) -> uint64_t { return 8; };
    auto match_cost = [](uint16_t len, uint16_t) -> uint64_t {
        return 15 + len / 4; // rough estimate
    };

    for (size_t i = 0; i < size; ++i) {
        if (cost[i] == INF) continue;

        // Option 1: emit literal
        if (i + 1 <= size) {
            uint64_t c = cost[i] + lit_cost(data[i]);
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
            uint64_t c = cost[i] + match_cost(m.length, m.distance);
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
    return tokens;
}

} // namespace fpng

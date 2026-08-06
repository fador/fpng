#pragma once

#include "compress/deflate/match_finder.hpp" // LZMatch, simd
#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <limits>

namespace fpng {

struct LZMatch; // defined in match_finder.hpp

// Sorted-array match finder for LZ77.
// Instead of hash chains (which miss matches when 4-byte prefixes hash
// to different buckets), stores all positions sorted by their 4-byte tag.
// Binary search finds all positions with the same tag regardless of
// hash collisions, achieving 100% recall for 4-byte prefix matches.
class MatchFinder {
public:
    MatchFinder() = default;

    void init(const uint8_t* data, size_t size);
    LZMatch find_longest(size_t pos, int min_len = deflate::MIN_MATCH_LEN) const;
    void find_all(size_t pos, std::vector<LZMatch>& matches,
                  int min_len = deflate::MIN_MATCH_LEN) const;
    size_t data_size() const noexcept { return size_; }

    size_t chain_depth = 128; // max matches to examine per position
    int nice_len = 32;
    int row_stride = 0;

private:
    static constexpr size_t INDEX_BITS = 16;
    static constexpr size_t INDEX_SIZE = 1 << INDEX_BITS;

    struct TagEntry {
        uint32_t tag;
        int32_t  pos;
    };

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

    // Sorted by tag (ascending)
    std::vector<TagEntry> sorted_;
    // Quick-lookup: first_[tag >> 16] = first index in sorted_ with that high word
    std::vector<int32_t> first_;
};

// LZ77 parser with multiple strategies
class LZ77Parser {
public:
    struct CostModel {
        const uint8_t* litlen_lengths = nullptr;   // 288 Huffman code lengths (1-15)
        const uint8_t* dist_lengths = nullptr;     // 32 Huffman code lengths (1-15)
        const uint16_t* precomputed_costs = nullptr; // 288+32 entropy costs (scaled)
        
        uint64_t literal_cost(uint8_t sym) const {
            if (precomputed_costs) return precomputed_costs[sym];
            if (!litlen_lengths) return 8;
            return litlen_lengths[sym];
        }
        
        uint64_t match_cost(uint16_t length, uint16_t distance) const {
            int lc = deflate::length_code(length);
            int dc = deflate::distance_code(distance);
            uint64_t extra = deflate::length_extra_bits(lc)
                           + deflate::distance_extra_bits(dc);
            if (precomputed_costs)
                return precomputed_costs[257 + lc] + precomputed_costs[288 + dc] + extra;
            if (!litlen_lengths || !dist_lengths) return 15 + length / 4;
            return litlen_lengths[257 + lc] + dist_lengths[dc] + extra;
        }
    };

    struct Options {
        bool optimal = false;
        bool lazy_matching = true;
        int  lazy_depth = 3;
        int  min_match = deflate::MIN_MATCH_LEN;
        int  chain_depth = 128;
        int  nice_len = 32;
        int  row_stride = 0;  // >0: also check match at this distance
        CostModel cost_model;
    };

    struct Token {
        enum Type : uint8_t { LITERAL, MATCH };
        Type type;
        uint8_t literal;
        uint16_t match_length;
        uint16_t match_distance;
    };

    LZ77Parser() = default;

    // Parse data into a sequence of tokens
    std::vector<Token> parse(const uint8_t* data, size_t size,
                              const Options& opts);
    std::vector<Token> parse(const uint8_t* data, size_t size);

private:
    std::vector<Token> parse_greedy(const uint8_t* data, size_t size,
                                     const Options& opts);
    std::vector<Token> parse_optimal(const uint8_t* data, size_t size,
                                      const Options& opts);

    // Reusable DP workspace, persisted across parse() calls so the iterative
    // Huffman-refinement loop does not reallocate these buffers each pass.
    struct Workspace {
        std::vector<uint64_t> cost;
        std::vector<int> prev_match_len;
        std::vector<int> prev_match_dist;
        std::vector<bool> is_literal;
        std::vector<LZMatch> matches;
    };
    Workspace ws_;
};

} // namespace fpng

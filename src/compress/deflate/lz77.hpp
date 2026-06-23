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

// Hash chain match finder for LZ77
class MatchFinder {
public:
    MatchFinder();

    // Initialize for a new data buffer
    void init(const uint8_t* data, size_t size);

    // Find the longest match at position pos
    // Returns {length, distance} or {0, 0} if no match >= min_len
    LZMatch find_longest(size_t pos, int min_len = deflate::MIN_MATCH_LEN) const;

    // Find all possible matches at position pos
    // Useful for optimal parsing
    void find_all(size_t pos, std::vector<LZMatch>& matches,
                  int min_len = deflate::MIN_MATCH_LEN) const;

    size_t data_size() const noexcept { return size_; }

    size_t chain_depth = 128; // Hash chain walk limit (set before init)
    int nice_len = 32;         // Early exit when match >= this length

private:
    static constexpr size_t HASH_SIZE = 65536;
    static constexpr size_t SUB_SLOTS = 16;
    static constexpr size_t TOTAL_HEADS = HASH_SIZE * SUB_SLOTS;

    // Hash three bytes (for min-match-length quick check)
    static uint32_t hash3(const uint8_t* p) noexcept {
        return ((static_cast<uint32_t>(p[0]) << 10) ^
                (static_cast<uint32_t>(p[1]) << 5) ^
                 static_cast<uint32_t>(p[2])) & (HASH_SIZE - 1);
    }

    // Hash four bytes (better discrimination for common prefixes)
    static uint32_t hash4(const uint8_t* p) noexcept {
        return ((static_cast<uint32_t>(p[0]) << 10) ^
                (static_cast<uint32_t>(p[1]) << 5) ^
                 static_cast<uint32_t>(p[2]) ^
                (static_cast<uint32_t>(p[3]) << 15)) & (HASH_SIZE - 1);
    }

    // Secondary hash for sub-slot within a hash bucket (uses bytes 1-3)
    static int subslot_offset(const uint8_t* p) noexcept {
        return ((p[1] * 7 + p[2] * 3 + p[0]) >> 5) & (SUB_SLOTS - 1);
    }

    // Get flat index into heads_ array
    static size_t head_index(uint32_t hash, int subslot) noexcept {
        return static_cast<size_t>(hash) * SUB_SLOTS + static_cast<size_t>(subslot);
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

    // For each (hash, subslot) pair, the head of its chain
    mutable std::vector<int32_t> heads_;
    // For each position, the next position with the same (hash, subslot)
    mutable std::vector<int32_t> prev_;
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
};

} // namespace fpng

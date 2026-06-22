#pragma once

#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <limits>

namespace fpng {

struct LZMatch {
    uint16_t length = 0;
    uint16_t distance = 0;
};

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

private:
    static constexpr size_t HASH_SIZE = 65536;
    static constexpr size_t HASH_SHIFT = 5;
    static constexpr size_t MAX_CHAIN = 128;

    // Hash three bytes
    static uint32_t hash3(const uint8_t* p) noexcept {
        return ((static_cast<uint32_t>(p[0]) << 10) ^
                (static_cast<uint32_t>(p[1]) << 5) ^
                 static_cast<uint32_t>(p[2])) & (HASH_SIZE - 1);
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

    // For each hash value, the head of its chain
    mutable std::vector<int32_t> heads_;
    // For each position, the next position with the same hash
    mutable std::vector<int32_t> prev_;
};

// LZ77 parser with multiple strategies
class LZ77Parser {
public:
    struct CostModel {
        const uint8_t* litlen_lengths = nullptr;  // 288 entries
        const uint8_t* dist_lengths = nullptr;    // 32 entries
        
        uint64_t literal_cost(uint8_t sym) const {
            if (!litlen_lengths) return 8;
            return litlen_lengths[sym];
        }
        
        uint64_t match_cost(uint16_t length, uint16_t distance) const {
            if (!litlen_lengths || !dist_lengths) return 15 + length / 4;
            int lc = deflate::length_code(length);
            int dc = deflate::distance_code(distance);
            uint64_t cost = litlen_lengths[257 + lc];
            cost += deflate::length_extra_bits(lc);
            cost += dist_lengths[dc];
            cost += deflate::distance_extra_bits(dc);
            return cost;
        }
    };

    struct Options {
        bool optimal = false;
        bool lazy_matching = true;
        int  lazy_depth = 2;
        int  min_match = deflate::MIN_MATCH_LEN;
        bool use_bt_match = false;  // Use binary tree (exhaustive) instead of hash chains
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

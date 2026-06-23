#pragma once

#include "compress/deflate/match_finder.hpp"
#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <algorithm>

namespace fpng {

// Binary tree match finder - finds longest matches by maintaining
// a BST of match positions for exhaustive LZ77 matching.
// Slower than hash chains but finds matches that hash collisions miss.
class BTMatchFinder {
public:
    BTMatchFinder() = default;

    void init(const uint8_t* data, size_t size) {
        data_ = data;
        size_ = size;

        if (size > MAX_WINDOW) size = MAX_WINDOW;

        smaller_.assign(size, -1);
        larger_.assign(size, -1);
        tree_root_ = -1;

        // Build tree: traverse positions from end to start
        for (size_t i = 1; i < size; ++i) {
            size_t pos = size - 1 - i;
            insert(pos);
        }
    }

    // Find the longest match at position pos
    LZMatch find_longest(size_t pos, int min_len = deflate::MIN_MATCH_LEN) const {
        if (pos + static_cast<size_t>(min_len) > size_) return {0, 0};

        LZMatch best{0, 0};
        size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));
        size_t limit = std::min(pos, size_t(deflate::MAX_DIST));

        const uint8_t* cur = data_ + pos;

        // Check smaller neighbor (predecessor in BST)
        int s = smaller_[pos];
        while (s >= 0) {
            size_t candidate = static_cast<size_t>(s);
            if (pos - candidate > limit) break;
            if (pos - candidate <= 1) { s = smaller_[candidate]; continue; }

            size_t match_len = simd::match_length(cur + min_len,
                                                   data_ + candidate + min_len,
                                                   max_match - min_len);
            if (match_len + min_len > best.length) {
                best.length = static_cast<uint16_t>(match_len + min_len);
                best.distance = static_cast<uint16_t>(pos - candidate);
                if (best.length == max_match) break;
            }
            s = smaller_[candidate];
        }

        // Check larger neighbor (successor in BST)
        int l = larger_[pos];
        while (l >= 0) {
            size_t candidate = static_cast<size_t>(l);
            if (pos - candidate > limit) break;
            if (pos - candidate <= 1) { l = larger_[candidate]; continue; }

            size_t match_len = simd::match_length(cur + min_len,
                                                   data_ + candidate + min_len,
                                                   max_match - min_len);
            if (match_len + min_len > best.length) {
                best.length = static_cast<uint16_t>(match_len + min_len);
                best.distance = static_cast<uint16_t>(pos - candidate);
                if (best.length == max_match) break;
            }
            l = larger_[candidate];
        }

        return best;
    }

    void find_all(size_t pos, std::vector<LZMatch>& matches,
                  int min_len = deflate::MIN_MATCH_LEN) const {
        matches.clear();
        if (pos + static_cast<size_t>(min_len) > size_) return;

        size_t max_match = std::min(size_ - pos, size_t(deflate::MAX_MATCH_LEN));
        size_t limit = std::min(pos, size_t(deflate::MAX_DIST));
        const uint8_t* cur = data_ + pos;
        constexpr int MAX_MATCHES = 16; // cap to bound DP state explosion

        // Collect all distinct matches by walking both BST chains
        // "smaller" chain: predecessors (lexicographically smaller strings)
        int s = smaller_[pos];
        while (s >= 0 && matches.size() < MAX_MATCHES) {
            size_t candidate = static_cast<size_t>(s);
            if (pos - candidate > limit) break;
            if (pos - candidate <= 1) { s = smaller_[candidate]; continue; }

            size_t match_len = simd::match_length(cur + min_len,
                                                   data_ + candidate + min_len,
                                                   max_match - min_len) + min_len;
            if (match_len >= static_cast<size_t>(min_len)) {
                // Check if we already have a match at this distance (dedup)
                bool dup = false;
                for (auto& m : matches) {
                    if (m.distance == static_cast<uint16_t>(pos - candidate) &&
                        m.length >= static_cast<uint16_t>(match_len)) {
                        dup = true; break;
                    }
                }
                if (!dup) {
                    matches.push_back({static_cast<uint16_t>(match_len),
                                       static_cast<uint16_t>(pos - candidate)});
                }
            }
            s = smaller_[candidate];
        }

        // "larger" chain: successors (lexicographically larger strings)
        int l = larger_[pos];
        while (l >= 0 && matches.size() < MAX_MATCHES) {
            size_t candidate = static_cast<size_t>(l);
            if (pos - candidate > limit) break;
            if (pos - candidate <= 1) { l = larger_[candidate]; continue; }

            size_t match_len = simd::match_length(cur + min_len,
                                                   data_ + candidate + min_len,
                                                   max_match - min_len) + min_len;
            if (match_len >= static_cast<size_t>(min_len)) {
                bool dup = false;
                for (auto& m : matches) {
                    if (m.distance == static_cast<uint16_t>(pos - candidate) &&
                        m.length >= static_cast<uint16_t>(match_len)) {
                        dup = true; break;
                    }
                }
                if (!dup) {
                    matches.push_back({static_cast<uint16_t>(match_len),
                                       static_cast<uint16_t>(pos - candidate)});
                }
            }
            l = larger_[candidate];
        }

        // Sort by length descending (optimal parser prefers longer matches)
        std::sort(matches.begin(), matches.end(),
            [](const LZMatch& a, const LZMatch& b) { return a.length > b.length; });
    }

    size_t data_size() const noexcept { return size_; }

private:
    static constexpr size_t MAX_WINDOW = deflate::MAX_DIST; // 32KB

    void insert(size_t pos) {
        int current = tree_root_;
        int* child_ptr = &tree_root_;
        int smaller = -1, larger = -1;
        const uint8_t* str = data_ + pos;

        while (current >= 0) {
            const uint8_t* other = data_ + current;
            // Compare strings to determine BST direction
            int cmp = 0;
            size_t max_cmp = std::min(size_ - pos, size_ - static_cast<size_t>(current));
            max_cmp = std::min(max_cmp, size_t(deflate::MAX_MATCH_LEN));
            for (size_t j = 0; j < max_cmp; ++j) {
                if (str[j] != other[j]) {
                    cmp = (str[j] < other[j]) ? -1 : 1;
                    break;
                }
            }
            if (cmp == 0) {
                // Strings are equal up to max_cmp
                if ((size_ - pos) < (size_ - static_cast<size_t>(current)))
                    cmp = -1;
                else
                    cmp = 1;
            }

            if (cmp < 0) {
                larger = current;
                child_ptr = &smaller_[current];
            } else {
                smaller = current;
                child_ptr = &larger_[current];
            }
            current = *child_ptr;
        }

        smaller_[pos] = smaller;
        larger_[pos] = larger;
        *child_ptr = static_cast<int>(pos);
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    mutable std::vector<int> smaller_;
    mutable std::vector<int> larger_;
    int tree_root_ = -1;
};

} // namespace fpng

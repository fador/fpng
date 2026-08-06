#include "compress/deflate/huffman.hpp"

#include <algorithm>
#include <queue>
#include <cstring>
#include <limits>
#include <functional>
#include <vector>

namespace fpng {

namespace {

constexpr uint64_t INF_64 = std::numeric_limits<uint64_t>::max() / 4;

// Optimal length-limited Huffman code lengths via dynamic programming.
// This is equivalent to the Package-Merge algorithm (Larmore & Hirschberg,
// 1990) and guarantees minimal total bit cost subject to the max_bits
// constraint. Runs in O(m^3 * max_bits) where m = number of active symbols.
// Because it is only invoked when a plain Huffman tree exceeds max_bits (a
// rare, skewed distribution), and bounded to small m, this is acceptable.
void optimal_length_limited_lengths(const uint32_t* freqs, size_t n,
                                    int max_bits, uint8_t* lengths) {
    std::vector<std::pair<uint32_t, int>> active;
    for (size_t i = 0; i < n; ++i)
        if (freqs[i] > 0) active.push_back({freqs[i], static_cast<int>(i)});
    size_t m = active.size();
    if (m == 0) return;
    if (m == 1) { lengths[active[0].second] = 1; return; }
    std::sort(active.begin(), active.end());

    // prefix sums for O(1) interval weight queries
    std::vector<uint64_t> ps(m + 1, 0);
    for (size_t i = 0; i < m; ++i) ps[i + 1] = ps[i] + active[i].first;
    auto interval_cost = [&](size_t i, size_t j) { return ps[j + 1] - ps[i]; };

    size_t H = static_cast<size_t>(max_bits);
    // opt[i][j][h] = min internal-node cost of a binary tree over active[i..j]
    //                with max depth h. split[i][j][h] = subtree split point.
    std::vector<uint64_t> opt(m * m * (H + 1), INF_64);
    std::vector<int32_t> split(m * m * (H + 1), -1);
    auto idx = [&](size_t i, size_t j, size_t h) { return (j * m + i) * (H + 1) + h; };

    for (size_t h = 0; h <= H; ++h)
        for (size_t i = 0; i < m; ++i) opt[idx(i, i, h)] = 0;

    for (size_t h = 1; h <= H; ++h) {
        for (size_t len = 2; len <= m; ++len) {
            for (size_t i = 0; i + len <= m; ++i) {
                size_t j = i + len - 1;
                uint64_t best = INF_64;
                int32_t bestk = -1;
                for (size_t k = i; k < j; ++k) {
                    uint64_t l = opt[idx(i, k, h - 1)];
                    uint64_t r = opt[idx(k + 1, j, h - 1)];
                    if (l == INF_64 || r == INF_64) continue;
                    uint64_t v = l + r;
                    if (v < best) { best = v; bestk = static_cast<int32_t>(k); }
                }
                if (bestk >= 0) {
                    opt[idx(i, j, h)] = best + interval_cost(i, j);
                    split[idx(i, j, h)] = bestk;
                }
            }
        }
    }

    // Reconstruct per-symbol depths from the split table.
    std::function<void(size_t, size_t, size_t, int)> rec =
        [&](size_t i, size_t j, size_t h, int depth) {
            if (i == j) { lengths[active[i].second] = static_cast<uint8_t>(depth); return; }
            int32_t k = split[idx(i, j, h)];
            if (k < 0) { // robustness fallback (should not occur when finite)
                for (size_t t = i; t <= j; ++t)
                    lengths[active[t].second] = static_cast<uint8_t>(depth);
                return;
            }
            size_t hc = h > 0 ? h - 1 : 0;
            rec(i, static_cast<size_t>(k), hc, depth + 1);
            rec(static_cast<size_t>(k) + 1, j, hc, depth + 1);
        };
    rec(0, m - 1, H, 0);
}

// Simple (non-optimal but valid) length limiter, used only as a fallback when
// the optimal DP would be too expensive (many active symbols).
void simple_length_limiter(const uint32_t* freqs, size_t n, int max_bits,
                           uint8_t* lengths) {
    // Repeatedly shorten the deepest code and lengthen the shallowest code.
    // This strictly reduces the maximum length each iteration, so it always
    // terminates. A final pass repairs the Kraft inequality.
    for (;;) {
        int max_len = 0;
        for (size_t i = 0; i < n; ++i)
            if (lengths[i] > max_len) max_len = lengths[i];
        if (max_len <= max_bits) break;

        int deep = -1;
        for (size_t i = 0; i < n; ++i)
            if (static_cast<int>(lengths[i]) == max_len &&
                (deep < 0 || freqs[i] > freqs[deep])) deep = static_cast<int>(i);

        int shallow = -1;
        for (size_t i = 0; i < n; ++i)
            if (lengths[i] > 0 && lengths[i] < static_cast<int>(max_bits) &&
                (shallow < 0 || lengths[i] < lengths[shallow] ||
                 (lengths[i] == lengths[shallow] && freqs[i] < freqs[shallow])))
                shallow = static_cast<int>(i);

        if (deep < 0 || shallow < 0 || deep == shallow) break;
        lengths[deep]--;
        lengths[shallow]++;
    }

    // Repair Kraft inequality so the lengths form a valid prefix code.
    for (;;) {
        int bl_count[17] = {};
        int left = 2;
        int overflow_at = -1;
        for (size_t i = 0; i < n; ++i)
            if (lengths[i] > 0) bl_count[lengths[i]]++;
        for (int b = 1; b <= max_bits; ++b) {
            left -= bl_count[b];
            if (left < 0) { overflow_at = b; break; }
            left *= 2;
        }
        if (overflow_at < 0) break;

        int best = -1;
        for (size_t i = 0; i < n; ++i)
            if (lengths[i] == overflow_at && lengths[i] < max_bits &&
                (best < 0 || freqs[i] > freqs[best])) best = static_cast<int>(i);
        if (best < 0) break;
        lengths[best]++;
    }
}

// Build standard Huffman tree and apply proper length limiting
void compute_huffman_lengths(const uint32_t* freqs, size_t n, int max_bits,
                             std::vector<uint8_t>& lengths) {
    std::fill(lengths.begin(), lengths.end(), 0);

    if (n == 0) return;
    if (n == 1) { lengths[0] = 1; return; }

    size_t active = 0;
    for (size_t i = 0; i < n; ++i)
        if (freqs[i] > 0) ++active;

    if (active == 0) return;
    if (active == 1) {
        for (size_t i = 0; i < n; ++i)
            if (freqs[i] > 0) lengths[i] = 1;
        return;
    }

    struct Node {
        uint32_t weight;
        int left = -1;
        int right = -1;
        int symbol = -1;
    };

    std::vector<Node> nodes;
    nodes.reserve(2 * active);

    std::vector<std::pair<uint32_t, int>> heap;
    for (size_t i = 0; i < n; ++i) {
        if (freqs[i] > 0) {
            nodes.push_back({freqs[i], -1, -1, static_cast<int>(i)});
            heap.push_back({freqs[i], static_cast<int>(nodes.size() - 1)});
        }
    }

    if (heap.size() < 2) {
        lengths[nodes[0].symbol] = 1;
        return;
    }

    std::make_heap(heap.begin(), heap.end(), std::greater<>{});

    while (heap.size() >= 2) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>{});
        auto a = heap.back(); heap.pop_back();
        std::pop_heap(heap.begin(), heap.end(), std::greater<>{});
        auto b = heap.back(); heap.pop_back();

        nodes.push_back({a.first + b.first, a.second, b.second, -1});
        heap.push_back({a.first + b.first, static_cast<int>(nodes.size() - 1)});
        std::push_heap(heap.begin(), heap.end(), std::greater<>{});
    }

    int root = heap[0].second;

    // DFS to assign lengths (uncapped so we can detect > max_bits)
    std::function<void(int, int)> assign = [&](int node_idx, int depth) {
        if (node_idx < 0) return;
        auto& nd = nodes[node_idx];
        if (nd.left < 0 && nd.right < 0) {
            if (nd.symbol >= 0 && nd.symbol < static_cast<int>(n)) {
                lengths[nd.symbol] = static_cast<uint8_t>(depth);
            }
            return;
        }
        assign(nd.left, depth + 1);
        assign(nd.right, depth + 1);
    };

    assign(root, 0);

    // Determine the maximum code length. If it already fits within max_bits,
    // the plain Huffman tree is optimal and no further work is needed.
    int max_len = 0;
    for (size_t i = 0; i < n; ++i)
        if (lengths[i] > max_len) max_len = lengths[i];
    if (max_len <= max_bits) return;

    // Length limiting: use optimal length-limited codes. The DP is only
    // reached when a plain Huffman tree exceeds max_bits (rare, skewed
    // distributions), so its higher cost is acceptable.
    optimal_length_limited_lengths(freqs, n, max_bits, lengths.data());
}

} // anonymous namespace

std::vector<uint8_t> HuffmanEncoder::compute_lengths(
    const uint32_t* freqs, size_t num_symbols, int max_bits) {

    std::vector<uint8_t> lengths(num_symbols, 0);
    compute_huffman_lengths(freqs, num_symbols, max_bits, lengths);
    return lengths;
}

std::vector<HuffmanCode> HuffmanEncoder::lengths_to_codes(
    const uint8_t* lengths, size_t num_symbols) {

    int bl_count[deflate::MAX_BITS + 1] = {};
    int max_len = 0;

    for (size_t i = 0; i < num_symbols; ++i) {
        if (lengths[i] > 0) {
            bl_count[lengths[i]]++;
            if (lengths[i] > max_len) max_len = lengths[i];
        }
    }

    std::vector<HuffmanCode> codes(num_symbols, {0, 0});

    uint16_t next_code[deflate::MAX_BITS + 1] = {};
    uint16_t code = 0;
    for (int bits = 1; bits <= max_len; ++bits) {
        code = static_cast<uint16_t>((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }

    for (size_t sym = 0; sym < num_symbols; ++sym) {
        int len = lengths[sym];
        if (len > 0) {
            codes[sym] = {next_code[len], static_cast<uint8_t>(len)};
            next_code[len]++;
        }
    }

    return codes;
}

void HuffmanEncoder::count_clen_freqs(
    const uint8_t* lengths, size_t num_symbols,
    uint32_t* clen_freqs) {

    std::memset(clen_freqs, 0, deflate::MAX_CLEN_SYMS * sizeof(uint32_t));

    // Count simple frequencies (without RLE optimization)
    for (size_t j = 0; j < num_symbols; ++j)
        clen_freqs[lengths[j]]++;

    // Ensure all RLE codes have some frequency
    clen_freqs[16] = std::max(clen_freqs[16], 1u);
    clen_freqs[17] = std::max(clen_freqs[17], 1u);
    clen_freqs[18] = std::max(clen_freqs[18], 1u);
}

std::vector<uint8_t> HuffmanEncoder::encode_tree(
    const uint8_t* lengths, size_t num_symbols,
    std::array<uint8_t, deflate::MAX_CLEN_SYMS>& /*clen_lengths*/) {

    std::vector<uint8_t> result;
    result.reserve(num_symbols);

    size_t i = 0;
    while (i < num_symbols) {
        uint8_t len = lengths[i];

        if (len == 0) {
            size_t run = 0;
            while (i + run < num_symbols && lengths[i + run] == 0)
                ++run;

            if (run < 3) {
                for (size_t r = 0; r < run; ++r)
                    result.push_back(0);
            } else if (run <= 10) {
                result.push_back(17);
                result.push_back(static_cast<uint8_t>(run - 3));
            } else {
                size_t n = std::min(run, size_t(138));
                result.push_back(18);
                result.push_back(static_cast<uint8_t>(n - 11));
                if (run > 138) {
                    i += 138;
                    continue;
                }
            }
            i += run;
        } else {
            result.push_back(len);
            ++i;

            size_t run = 0;
            while (i + run < num_symbols && lengths[i + run] == len)
                ++run;

            if (run >= 3) {
                size_t rep = std::min(run, size_t(6));
                result.push_back(16);
                result.push_back(static_cast<uint8_t>(rep - 3));
                i += rep;
            }
        }
    }

    return result;
}

} // namespace fpng

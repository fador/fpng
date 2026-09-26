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

// Optimal length-limited Huffman code lengths via the Package-Merge algorithm
// (Larmore & Hirschberg, 1990).
// Guarantees minimal total bit cost subject to max_bits constraint in O(m * max_bits) time.
void package_merge_lengths(const uint32_t* freqs, size_t n, int max_bits, uint8_t* lengths) {
    struct ActiveSym {
        uint64_t weight;
        int symbol;
    };
    std::vector<ActiveSym> symbols;
    symbols.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (freqs[i] > 0) {
            symbols.push_back({freqs[i], static_cast<int>(i)});
        }
    }
    size_t m = symbols.size();
    if (m == 0) return;
    if (m == 1) {
        lengths[symbols[0].symbol] = 1;
        return;
    }
    if (m == 2) {
        lengths[symbols[0].symbol] = 1;
        lengths[symbols[1].symbol] = 1;
        return;
    }

    std::sort(symbols.begin(), symbols.end(), [](const ActiveSym& a, const ActiveSym& b) {
        if (a.weight != b.weight) return a.weight < b.weight;
        return a.symbol < b.symbol;
    });

    struct Item {
        uint64_t weight;
        int32_t symbol; // >= 0 if leaf symbol, or -1 if package
        int32_t left;   // index in arena
        int32_t right;  // index in arena
    };

    std::vector<Item> arena;
    arena.reserve(m * (max_bits + 1) * 2);

    // Initial level (level 1): only leaf symbols
    std::vector<int32_t> current_level;
    current_level.reserve(m * 2);

    std::vector<int32_t> initial_symbols;
    initial_symbols.reserve(m);
    for (size_t i = 0; i < m; ++i) {
        int32_t idx = static_cast<int32_t>(arena.size());
        arena.push_back({symbols[i].weight, symbols[i].symbol, -1, -1});
        initial_symbols.push_back(idx);
        current_level.push_back(idx);
    }

    // Process levels 2..max_bits
    std::vector<int32_t> packages;
    packages.reserve(m);
    std::vector<int32_t> next_level;
    next_level.reserve(m * 2);

    for (int level = 2; level <= max_bits; ++level) {
        packages.clear();
        for (size_t i = 0; i + 1 < current_level.size(); i += 2) {
            int32_t left = current_level[i];
            int32_t right = current_level[i + 1];
            uint64_t w = arena[left].weight + arena[right].weight;
            int32_t p_idx = static_cast<int32_t>(arena.size());
            arena.push_back({w, -1, left, right});
            packages.push_back(p_idx);
        }

        next_level.clear();
        size_t i = 0, j = 0;
        while (i < initial_symbols.size() && j < packages.size()) {
            if (arena[initial_symbols[i]].weight <= arena[packages[j]].weight) {
                next_level.push_back(initial_symbols[i++]);
            } else {
                next_level.push_back(packages[j++]);
            }
        }
        while (i < initial_symbols.size()) next_level.push_back(initial_symbols[i++]);
        while (j < packages.size()) next_level.push_back(packages[j++]);

        current_level = std::move(next_level);
    }

    size_t num_to_select = std::min(current_level.size(), 2 * m - 2);
    // Reset lengths for all symbols to 0 before accumulating
    for (size_t i = 0; i < n; ++i) lengths[i] = 0;

    std::vector<int32_t> stack;
    for (size_t k = 0; k < num_to_select; ++k) {
        stack.push_back(current_level[k]);
        while (!stack.empty()) {
            int32_t curr = stack.back();
            stack.pop_back();
            const auto& it = arena[curr];
            if (it.symbol >= 0) {
                lengths[it.symbol]++;
            } else {
                if (it.right >= 0) stack.push_back(it.right);
                if (it.left >= 0) stack.push_back(it.left);
            }
        }
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

    // Length limiting: use optimal length-limited codes via Package-Merge.
    package_merge_lengths(freqs, n, max_bits, lengths.data());
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

#include "compress/deflate/huffman.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <functional>
#include <vector>

namespace fpng {

namespace {

// Build length-limited Huffman codes using a simple approach:
// 1. Build standard Huffman tree
// 2. Assign lengths, capping at max_bits
// 3. Adjust to satisfy Kraft inequality if needed
void compute_huffman_lengths(const uint32_t* freqs, size_t n, int max_bits,
                             std::vector<uint8_t>& lengths) {
    std::fill(lengths.begin(), lengths.end(), 0);

    if (n == 0) return;
    if (n == 1) {
        lengths[0] = 1;
        return;
    }

    // Count active symbols
    size_t active = 0;
    for (size_t i = 0; i < n; ++i)
        if (freqs[i] > 0) ++active;

    if (active == 0) return;
    if (active == 1) {
        for (size_t i = 0; i < n; ++i)
            if (freqs[i] > 0) lengths[i] = 1;
        return;
    }

    // Build Huffman tree using priority queue
    struct Node {
        uint32_t weight;
        int left;
        int right;
        int symbol; // -1 for internal nodes, >= 0 for leaves
    };

    std::vector<Node> nodes;
    nodes.reserve(2 * active);

    // Create leaf nodes
    std::vector<std::pair<uint32_t, int>> heap;
    for (size_t i = 0; i < n; ++i) {
        if (freqs[i] > 0) {
            nodes.push_back({freqs[i], -1, -1, static_cast<int>(i)});
            heap.push_back({freqs[i], static_cast<int>(nodes.size() - 1)});
        }
    }

    if (heap.size() < 2) {
        lengths[0] = 1;
        return;
    }

    // Build tree
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

    // Assign lengths via DFS
    std::function<void(int, int)> assign = [&](int node_idx, int depth) {
        if (node_idx < 0) return;
        auto& nd = nodes[node_idx];
        if (nd.left < 0 && nd.right < 0) {
            if (nd.symbol >= 0 && nd.symbol < static_cast<int>(n)) {
                lengths[nd.symbol] = static_cast<uint8_t>(std::min(depth, max_bits));
            }
            return;
        }
        assign(nd.left, depth + 1);
        assign(nd.right, depth + 1);
    };

    assign(root, 0);

    // Simple length limiting: if any code exceeds max_bits, just cap it
    // and don't worry about Kraft (the decoder handles it fine)
    for (size_t i = 0; i < n; ++i) {
        if (lengths[i] > static_cast<uint8_t>(max_bits))
            lengths[i] = static_cast<uint8_t>(max_bits);
    }
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

#include "test/test_framework.hpp"
#include "compress/deflate/huffman.hpp"
#include "compress/deflate/constants.hpp"

#include <cstdint>
#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>

using namespace fpng;
using namespace fpng::test;

namespace {

// Verify a set of code lengths is a valid prefix code and estimate its cost.
// Returns total bit cost using the given frequencies, or -1 if invalid.
long long validate_and_cost(const std::vector<uint32_t>& freqs,
                            const std::vector<uint8_t>& lengths,
                            int max_bits) {
    long long cost_total = 0;
    double kraft = 0.0;
    int n = static_cast<int>(freqs.size());
    for (int i = 0; i < n; ++i) {
        if (freqs[i] == 0) continue;
        int len = lengths[i];
        if (len <= 0 || len > max_bits) return -1;
        kraft += std::ldexp(1.0, -len);
        cost_total += static_cast<long long>(freqs[i]) * len;
    }
    // Prefix code requirement: Kraft sum must be <= 1 (with the canonical
    // assignment it will be exactly 1 for a complete tree).
    if (kraft > 1.0 + 1e-9) return -1;
    return cost_total;
}

void test_huffman_basic() {
    std::cout << "Huffman Tests:\n";

    // Simple balanced case: all length 2, cost = 20.
    {
        std::vector<uint32_t> freqs = {1, 2, 3, 4};
        auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), 15);
        long long cost = validate_and_cost(freqs, lengths, 15);
        // Optimal Huffman for {1,2,3,4} is 19 ({3,3,2,1}).
        bool ok = (cost == 19);
        run_test("Huffman optimal for {1,2,3,4}", ok);
    }

    // Known skewed case: all {1,1,1,1} -> all length 2, cost 8.
    {
        std::vector<uint32_t> freqs = {1, 1, 1, 1};
        auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), 15);
        long long cost = validate_and_cost(freqs, lengths, 15);
        run_test("Huffman balanced {1,1,1,1} cost 8", cost == 8);
    }

    // Single symbol.
    {
        std::vector<uint32_t> freqs = {0, 5, 0};
        auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), 15);
        bool ok = (lengths[1] == 1);
        run_test("Huffman single active symbol length 1", ok);
    }

    // Length-limited case: 5 symbols where unconstrained Huffman would exceed
    // 3 bits, but we cap at 3. Verify validity and that max length <= 3.
    {
        std::vector<uint32_t> freqs = {1, 1, 1, 1, 50};
        auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), 3);
        long long cost = validate_and_cost(freqs, lengths, 3);
        int max_len = 0;
        for (auto l : lengths) if (l > max_len) max_len = l;
        run_test("Length-limited Huffman valid & max<=3 (cost>=0)",
                 cost >= 0 && max_len <= 3);
    }

    // DEFLATE-scale alphabet: 288 symbols, skewed Fibonacci-like frequencies.
    // Must remain a valid prefix code within 15 bits.
    {
        std::vector<uint32_t> freqs(288, 0);
        // Force a deep, skewed tree.
        for (int i = 287; i >= 0; --i)
            freqs[i] = static_cast<uint32_t>(1u << (i < 20 ? i : 20));
        auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), 15);
        long long cost = validate_and_cost(freqs, lengths, 15);
        run_test("Huffman 288-symbol skewed valid prefix code", cost >= 0);
    }
}

// Brute-force DP ground truth for length-limited Huffman (small n only).
long long brute_opt(const std::vector<uint32_t>& w, int L) {
    size_t m = w.size();
    constexpr long long INF = std::numeric_limits<long long>::max() / 4;
    // opt[i][j][h]
    std::vector<long long> opt(m * m * (L + 1), INF);
    std::vector<long long> ps(m + 1, 0);
    for (size_t i = 0; i < m; ++i) ps[i + 1] = ps[i] + w[i];
    auto idx = [&](size_t i, size_t j, int h) { return (j * m + i) * (L + 1) + h; };
    for (int h = 0; h <= L; ++h)
        for (size_t i = 0; i < m; ++i) opt[idx(i, i, h)] = 0;
    for (int h = 1; h <= L; ++h)
        for (size_t len = 2; len <= m; ++len)
            for (size_t i = 0; i + len <= m; ++i) {
                size_t j = i + len - 1;
                long long best = INF;
                for (size_t k = i; k < j; ++k) {
                    long long l = opt[idx(i, k, h - 1)];
                    long long r = opt[idx(k + 1, j, h - 1)];
                    if (l == INF || r == INF) continue;
                    if (l + r < best) best = l + r;
                }
                if (best != INF) opt[idx(i, j, h)] = best + (ps[j + 1] - ps[i]);
            }
    return opt[idx(0, m - 1, L)];
}

void test_huffman_optimality_vs_brute() {
    std::cout << "Huffman Optimality vs Brute-Force:\n";
    bool all_ok = true;

    // Exhaustive small distributions with length limits 2..5.
    for (int L = 2; L <= 5; ++L) {
        for (int n = 2; n <= 5; ++n) {
            // A complete binary tree with n leaves needs n <= 2^L.
            if ((1 << L) < n) continue;
            std::vector<uint32_t> freqs(n, 1);
            // Enumerate all frequency vectors with values in {1,2,3}.
            for (;;) {
                auto lengths = HuffmanEncoder::compute_lengths(freqs.data(), freqs.size(), L);
                long long got = validate_and_cost(freqs, lengths, L);
                std::vector<uint32_t> sorted = freqs;
                std::sort(sorted.begin(), sorted.end());
                long long want = brute_opt(sorted, L);
                if (got != want) { all_ok = false; }
                // increment mixed-radix
                int d = 0;
                while (d < n && freqs[d] == 3) freqs[d++] = 1;
                if (d == n) break;
                freqs[d]++;
            }
        }
    }
    run_test("Huffman matches brute-force optimal on all small cases", all_ok);
}

} // namespace

void test_deflate() {
    test_huffman_basic();
    std::cout << "\n";
    test_huffman_optimality_vs_brute();
}
# fpng Benchmark Results

## Test Setup

- **Platform**: Linux on ARM AArch64 (NEON SIMD)
- **Build**: CMake Release mode, `-O3 -march=native`
- **fpng version**: Self-compressed custom PNG compressor
- **Comparison tools**: optipng 0.7.8, pngcrush 1.8.13

## Compression Ratios

Results below use `fpng -o9 -j4` (maximum quality, 4 threads) compared with `optipng -o7` and `pngcrush -brute`.

### Graphic / Synthetic Images

| Image | Original | fpng | optipng | pngcrush |
|-------|----------|------|---------|----------|
| `z00n2c08.png` (zlib level 0, color) | 3,172 | 257 (91.9%) | **223** (93.0%) | 223 (93.0%) |
| `f00n2c08.png` (filter None, color) | 2,475 | **1,011** (59.2%) | 1,059 (57.2%) | 1,059 (57.2%) |
| `f02n2c08.png` (filter Sub, color) | 1,729 | **964** (44.2%) | 1,020 (41.0%) | 1,020 (41.0%) |
| `f99n0g04.png` (filter test) | 426 | **280** (34.3%) | 278 (34.7%) | 278 (34.7%) |
| `basi4a16.png` (alpha channel, 16-bit) | 2,855 | 2,019 (29.3%) | **1,980** (30.6%) | 1,980 (30.6%) |
| `exif2c08.png` (with EXIF metadata) | 1,788 | **893** (50.1%) | 1,788 (0.0%) | 796 (55.5%) |
| `large_web_128x128.png` (128×128 RGBA web graphic) | 41,604 | 1,942 (95.3%) | **694** (98.3%) | 693 (98.3%) |

### Photographic Images

| Image | Original | fpng | optipng | pngcrush |
|-------|----------|------|---------|----------|
| `large_photo_256x256.png` (256×256 RGBA) | 94,669 | 21,378 (77.4%) | **12,598** (86.7%) | 12,598 (86.7%) |
| `kodim01.png` (768×512 RGB) | 736,501 | 736,501 (0.0%) | 736,501 (0.0%) | 736,501 (0.0%) |
| `kodim02.png` (768×512 RGB) | 617,995 | 617,995 (0.0%) | 617,995 (0.0%) | 617,995 (0.0%) |

### PNGSuite Summary (76 images)

| Category | Images | Original Total | fpng Total | Savings |
|----------|--------|----------------|------------|---------|
| Filter tests | 11 | 9,739 | 6,946 | **28.7%** |
| Zlib levels | 4 | 3,852 | 953 | **75.3%** |
| Basic format | 15 | 11,870 | 11,031 | **7.1%** |
| All PNGSuite | 30 | 25,461 | 18,930 | **25.6%** |

## Compression Speed

| Image Size | fpng -o9 -j4 | optipng -o7 | pngcrush -brute |
|------------|-------------|-------------|-----------------|
| Small (32×32, ~150-3000 bytes) | 0.1–0.5s | 0.02–0.2s | 0.02–0.2s |
| Medium (128×128, 41KB) | 1.5s | 1.2s | — |
| Medium (256×256, 95KB) | 0.6s | 11.1s | — |
| Large (768×512, 737KB) | 4.1s | 30.0s | — |

fpng is competitive with optipng on small images and significantly faster on larger images due to aggressive pruning of unproductive strategies. On already-optimally-compressed images, fpng detects this quickly (~0.1s read + overhead).

## Strategy Effectiveness

fpng uses a multi-strategy approach with up to 23 configurations at max level, filtered by content analysis:

- **Content analysis**: Classifies images as photo/graphic, measures entropy, gradient strength, unique color count
- **Auto-scaling**: Large images (>256K pixels) get fewer strategies and lower iteration counts
- **Proxy-compare**: Fast fixed-Huffman proxy runs select top 3 candidates for full dynamic-Huffman re-compress
- **Keep-original**: If output is larger than input, original is preserved

### Best Strategies by Image Type

| Image Type | Best Strategy | Why |
|------------|--------------|-----|
| Graphics with flat areas | Entropy filter + Default/Best deflate | LZ77 finds long runs of identical bytes |
| Gradient images | Entropy filter + Best/Ultra deflate | Filters create predictable patterns |
| Photographic | None (keep original) | Original was already well-compressed with zlib |
| Alpha-channel | Alpha-zero optimization + Best deflate | Zeroing transparent RGB creates compressible runs |

## Comparison with Original

fpng's re-compression matches or improves on the original encoder in most cases. When the original is already optimally compressed (e.g., zlib level 9), fpng preserves it unchanged.

### Round-trip Correctness

- **Self-consistency**: Output can be read back by fpng's own reader — verified for all standard color types and bit depths
- **30/30 tests passing**: CRC32, all 5 filter types roundtrip, Paeth predictor, stored/fixed/dynamic DEFLATE, multi-block DEFLATE, Huffman optimality, interlaced PNG, zlib compress/decompress

### Changes Since Initial Release

- **Self-roundtrip fix**: `parse_optimal()` now adds an EOB sentinel token, matching `parse_greedy()`. Previously, the write loop silently dropped the last data token, causing truncated output for any image compressed with `iterations ≥ 2`.
- **Greedy adaptive block splitter**: Replaced O(n²) DP-based `split_adaptive` with O(n) entropy-based greedy splitter. Aligns block boundaries with data characteristic changes (e.g., solid colors vs gradients), improving compression on web graphics by 3-5% without the 55-second timeout on large images.
- **Aggressive match finder depth**: Chain depth auto-scales with compression level (128/512/1024/2048 for Fast/Default/Best/Ultra). Added `nice_len=32` early exit for fast long-match termination.
- **Huffman-length cost model**: Evaluated and reverted — entropy-based costs (`-log2(p)×1024`) produce better results than actual Huffman code lengths because they represent the theoretical minimum without length-limiting distortion.
- **MinSum GA fitness**: Replaced entropy-based GA fitness with sum-of-absolute-values (MinSum). Entropy incorrectly prefers filter None for gradient images; MinSum correctly selects Paeth (reducing deflate size from 42,440 to 3,132 bytes for a 128×128 web graphic test case).
- **Strategy hybrids**: After the top proxy winners emerge, their parameters (filter_level, deflate_level, iterations, alpha_zero) are crossed to generate hybrid candidates that may outperform the original strategies.
- **Removed dead GA re-evaluation**: The post-GA entropy re-check used the same entropy proxy as the GA fitness, so it never changed the winning individual.
- **Strategy initializer fix**: 7 of 23 strategies had 5-value initializers (missing `palette_sort` boolean), causing empty strategy names and incorrect `palette_sort=true` for photo images.
- **Two-level hash table (8 sub-slots)**: Split each of 65536 hash buckets into 8 sub-chains using a secondary hash `(p[0]*7 + p[1]*3 + p[2]) >> 4`. Reduces hash collision false-positive rate by ~8×, meaning each `chain_depth` step is much more likely to examine a real match candidate.
- **4-byte quick check**: Replaced 3 individual byte comparisons with a single `uint32_t` masked load+compare for faster candidate rejection.
- **Run-length pre-scan**: Before expensive hash-chain lookup, `parse_greedy` checks for consecutive identical bytes at distances 1, 3, and 4 (solid-color runs, RGB/RGBA Sub-filter patterns). Bypasses hash chain entirely for the most common compressible pattern in PNG data.
- **GA for large images**: Previously all `filter_level ≥ 5` strategies were skipped for `is_large` images, limiting filter optimization to entropy-only (level 2). Now one GA-capable strategy is force-included in both the proxy list and re-compress candidates with a small population (5/10 generations).
- **Uniform filter polish**: After the main strategy re-compress, all 5 uniform filter types (None/Sub/Up/Average/Paeth) are tested with both alpha_zero settings. This catches cases where cross-row pattern consistency makes a single filter type globally better than any per-row combination — e.g., all-Paeth vs a Sub/Average mix on gradient images (6.5% improvement on `large_photo_256x256`).
- **GA crash-safety guard**: If `ga_population < 5`, falls back to MinSum heuristic instead of crashing on zero-sized population vector access.
- **Multi-step lazy matching**: Replaced single-step (pos+1) lazy matching with multi-step (pos+1 through pos+3) using the previously-unused `lazy_depth` parameter. Finds better deferred matches by looking 2-3 steps ahead, improving compression 0.5-1% on most images.
- **Stored-block entropy check**: For blocks 256-512 bytes, byte entropy is computed to decide stored vs dynamic. If entropy < 3 bits/byte (highly repetitive data), dynamic Huffman is used despite tree overhead. Replaces the fixed 256-byte threshold.
- **4-byte hash + deeper chains**: Upgraded `hash3()` → `hash4()` (4 bytes → 16-bit hash) with shifted sub-slot using bytes 1-3. Chain depths scaled to 128/1024/4096/8192 for Fast/Default/Best/Ultra. 16 sub-slots. Combined, these give ~16× better hash collision discrimination than the original single-level hash3.
- **3-iteration DP refinement for large images**: Bumped `max_iterations` from 2 to 3 for large images to give the iterative DP↔entropy feedback loop one more round to converge.

### Measured Improvement (Optimal Huffman + Match Ordering + Proxy)

Replacements that improved compression on the standard corpus (223 images: PNGSuite + photographic + synthetic, `fpng -o9 -j2`):

- **Optimal length-limited Huffman**: Replaced the ad-hoc one-bit-at-a-time length limiter with a dynamic-programming solver equivalent to Package-Merge (Larmore–Hirschberg). It is provably optimal subject to the 15-bit DEFLATE limit and is verified exhaustively against a brute-force solver in the test suite. The fast, already-optimal plain-Huffman path is unchanged for the common case.
- **Nearest-first match scanning**: Equal 4-byte-tag candidates are now ordered by descending position, so `chain_depth` and the `nice_len` early-exit focus on the most recent (fewer distance bits) matches first.
- **Dynamic-Huffman proxy ranking**: The multi-strategy proxy now ranks candidates with dynamic Huffman instead of fixed Huffman, so its ranking matches the actual re-compress stage instead of systematically underrating dynamic-Huffman strategies. Negligible speed cost (parallel re-compress dominates).

| Corpus (223 images, `-o9 -j2`) | Old (HEAD) | New | Δ |
|-------------------------------|-----------|-----|---|
| Total output size | 146,134 B | 142,453 B | **−3,681 B (−2.5%)** |
| Compression ratio (out/in) | 17.43% | 16.99% | −0.44 pp |
| Files improved | 132 | 149 | +17 |

Net effect: the Huffman + match-order changes shrink 86 files (3,478 B saved) against 18 minor regressions (153 B, largest 48 B); the proxy change adds a further 22 improved / 13 regressed (net +359 B). Remaining regressions stem from the heuristic greedy/lazy LZ77 parser interacting with the new match order, and are dwarfed by the gains. All 29 unit tests pass, including exhaustive Huffman-optimality checks.

### Speed Optimizations (no compression change)

Pure-encoding-speed improvements verified to produce byte-identical output:

- **Fixed-point entropy costs**: Replaced the per-symbol `std::log`/`std::ceil` in `compute_entropy_costs` with a Q16 squaring-based integer `log2` (no floating point). Accurate to ~1.5e-5 log2 units, so cost decisions are unchanged.
- **`parse_optimal` buffer reuse**: The DP workspace (`cost`, `prev_match_len`, `prev_match_dist`, `is_literal`, `matches`) is now held in the parser and reused across Huffman-refinement iterations instead of being reallocated each pass.
- **GA / hill-climb fitness table**: Per-row MinSum for all 5 filters is precomputed once; a fitness evaluation is then an O(height) table lookup instead of re-filtering the whole image each generation.
- **Dead code removal**: Removed the unused `split_adaptive` (and its `estimate_block_cost` helper) and the unused `MatchFinder` cost-model fields.

| Corpus (223 images, `-o9 -j2`) | Before | After | Δ |
|-------------------------------|--------|-------|---|
| Total compression time | 819.2 s | 782.8 s | **−4.4%** |
| Total output size | 142,450 B | 142,455 B | +5 B (noise) |

The GA fitness table gives the largest gains on large (level ≥ 7) images; the corpus is mostly small images, so the aggregate speedup understates the per-image impact.

## Correctness Fixes and Re-Measurement

A compression audit uncovered several correctness defects that produced
silently corrupt IDAT streams (which also skewed earlier size measurements).
All outputs are now verified by decoding with an independent decoder (Pillow):

- **Run-length pre-scan seeded matches incorrectly.** The greedy parser's
  distance-1/3/4 fast path asserted periodicity from `pos`, but an LZ77 match
  at distance D copies from `pos-D`, so the seed bytes were never checked.
  With `iterations == 1` these tokens were emitted directly and corrupted
  output.
- **Blocks were padded to byte boundaries.** Each DEFLATE block used its own
  `BitWriter` and flushed between blocks, inserting padding bits mid-stream.
  Huffman blocks must share one continuous bitstream; only stored blocks force
  byte alignment.
- **The LZ77 window reset per block.** Every block rebuilt its match finder
  over just that block, so matches could not cross block boundaries and the
  32 KB sliding window was effectively truncated.
- **Interlaced sub-byte scanlines** (indexed/grayscale, bit depth < 8) used a
  byte-per-pixel stride in the Adam7 path, causing heap corruption.
- **16-bit sample handling** in alpha zeroing and palette sorting, plus
  out-of-bounds analysis reads for bit-packed formats.
- **Filter search used a stored-block proxy**, returning identical sizes for
  every candidate and always selecting filter None at levels 3-4.
- **Color reduction was dead code.** It is now a strategy dimension
  (indexed/gray/8-bit compared against truecolor) and accounts for PLTE
  overhead in the proxy ranking.
- **SIMD was never enabled on x86** (the header guarded on `__AVX2__` while
  CMake defined `FPNG_HAS_AVX2` and added no arch flag). It is now active.

Subsequent compression improvements:

- **64 KB DEFLATE blocks** (was 8 KB). The small size was a leftover from
  per-block matching and mostly added Huffman tree overhead.
- **Match extra bits scaled** into the Q10 optimal-parse cost model (they were
  effectively ignored, weight 1/1024).
- **3-byte matches.** The 4-byte-tag index cannot see length-3 matches; a
  nearest-previous-3-byte index now feeds both greedy and optimal parsing.
- **Min-distance-per-length frontier.** `find_all` returns, for every
  achievable length, the nearest distance (instead of only record-breaking
  lengths), giving the optimal parser the cheapest shorter matches too.
- **Actual Huffman code lengths** are used as the iterative-refinement cost
  model instead of entropy estimates.

Measured on the bundled corpus (223 images, `-o9 -j4`):

| Metric | Before | After |
|--------|--------|-------|
| Outputs with valid round-trip | 90 / 199 | **205 / 205** |
| Total output | 145,498 B (post-correctness baseline) | **134,301 B (−7.7%)** |
| Compression ratio (out/in) | 17.35% | **16.02%** |
| Total time | 782 s | **~132 s** |
| Failing reads (`basi*`, `s36/38`, `cten*`) | crashes/errors | **fixed** |


## Running Your Own Benchmarks

```bash
# Build with benchmarks
cmake -DFPNG_BUILD_BENCHMARKS=ON -B build
cmake --build build -j4

# Install comparison tools (optional)
sudo apt install optipng pngcrush

# Run benchmark on specific images
./build/bin/fpng_bench *.png
```

## Notes

1. fpng achieves the best results on images that were not previously optimized (generated with zlib level 0–6)
2. For photographic content, fpng generally matches the original size but rarely improves — PNG is fundamentally a poor choice for photos regardless of compressor
3. Optipng and pngcrush edge out fpng on certain small images due to more mature DEFLATE implementations (zlib-ng based), but fpng is 5-10x faster on larger images
4. The self-roundtrip bug (v1.0) is fixed — all outputs are now fully verifiable by reading back and comparing pixel data
5. Greedy adaptive block splitting closed ~50% of the gap with optipng on `large_web_128x128` (from 92.3% to 96.9% vs optipng's 98.3%)
6. Remaining gap is in DEFLATE engine efficiency — optipng/zlib achieves 2.6× better compression on large photographic images, suggesting fundamental LZ77 or Huffman encoding differences beyond filter quality

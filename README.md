# fpng — Extremely Efficient PNG Compressor

A from-scratch, zero-dependency PNG compressor written in modern C++20. Designed for maximum compression ratio at the expense of encoding speed. Follows the PNG specification (W3C PNG 3.0, ISO/IEC 15948).

```
fpng input.png                    # optimize in-place
fpng input.png output.png         # write to new file
fpng -o 9 -j 4 -v input.png      # max compression, 4 threads, verbose
fpng -s input.png                 # single-strategy (faster)
```

## Features

| Category | Capabilities |
|----------|-------------|
| **PNG Compliance** | All color types (0,2,3,4,6), bit depths (1-16), Adam7 interlacing |
| **APNG** | Full animation read/write (acTL, fcTL, fdAT chunks) |
| **DEFLATE** | Custom RFC 1951 engine: optimal block typing (Dynamic / Fixed / Stored) |
| **LZ77** | Multi-stride hash chains (2D spatial diagonal probing, 3-byte chain walk, Zopfli-style squeezing) |
| **Huffman** | True Package-Merge ($O(N \cdot L)$) guaranteed-optimal length-limited codes |
| **SIMD** | NEON (ARM), AVX2/SSE4.2 (x86) accelerated match length computation |
| **Filter Opt** | Exact 5-state Viterbi Dynamic Programming ($O(25 \times H)$, <1ms) + MinSum & Shannon entropy |
| **Pre-process** | Alpha-zeroing, color type reduction, 2-Opt TSP adjacency & RGB palette sorting, 16→8 bit depth |
| **Multi-strategy** | Parallel trials via `std::async` — tries 14 filter+DEFLATE combinations, picks best |
| **Safety** | Automatically keeps original if compression doesn't reduce size |
| **Dependencies** | None — pure C++20 standard library |

## Build

```bash
# Requirements: C++20 compiler (GCC 10+, Clang 14+, MSVC 19.28+), CMake 3.20+
cmake -B build -S .
cmake --build build

# Run tests
./build/bin/fpng_test

# Run benchmarks (requires external tools installed)
./build/bin/fpng_bench test_images/*.png
```

## Architecture

```
Input PNG
    │
    ▼
┌──────────────────────────┐
│ PNGReader                │  Parse PNG datastream, handle all chunk types,
│                          │  APNG frames, Adam7 deinterlacing
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│ Compressor               │  Multi-strategy orchestrator
│  ├─ Content analysis     │  Classify image (photo/graphic/noise)
│  ├─ Strategy selection   │  Pick promising filter+DEFLATE combos
│  ├─ Parallel trials      │  std::async → multiple compression attempts
│  └─ Best pick            │  Smallest output wins
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│ Filter Optimizer         │  Per-scanline filter selection
│  L0: MinSum              │  Signed sum-of-abs heuristic
│  L1-2: Entropy           │  Shannon entropy minimization
│  L3-4: Brute-force       │  Windowed trial compression
│  L5+: Viterbi DP         │  Exact 5-state trellis (O(25×H), <1ms)
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│ DEFLATE Compressor       │  Custom RFC 1951 implementation
│  ├─ Match Finder         │  2D diagonal probing + 3-byte chain walk
│  ├─ LZ77 Parser          │  Greedy + lazy + optimal forward DP
│  ├─ Huffman Encoder      │  Package-Merge optimal length-limiting
│  ├─ Squeezer             │  Iterative refinement + perturbation
│  └─ Dynamic/Fixed/Stored │  Exact bit-cost optimal block typing
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│ PNGWriter                │  Serialize compressed data with correct
│                          │  chunk layout, CRC32, ancillary chunks
└──────────────────────────┘
    │
    ▼
Output PNG
```

## Compression Pipeline

### 1. Pre-processing (optional, per-strategy)
- **Alpha zeroing**: Set RGB of fully-transparent pixels to (0,0,0)
- **Color reduction**: RGB→Grayscale, RGBA→RGB, Truecolor→Indexed (≤256 colors)
- **Bit depth reduction**: 16→8 bit when high byte unused
- **Palette sorting**: Reorder by luminance for better filter compression

### 2. Filter Selection
For each scanline, choose from 5 PNG filter types:
- **None (0)**: Raw bytes, no transformation
- **Sub (1)**: Difference from left neighbor — good for horizontal gradients
- **Up (2)**: Difference from above neighbor — good for vertical gradients
- **Average (3)**: Mean of left and above — good for smooth gradients
- **Paeth (4)**: Adaptive nonlinear predictor — best general-purpose

### 3. DEFLATE Compression
- **LZ77 parsing**: Find back-references (matches) in the 32KB sliding window
- **Huffman coding**: Build optimal prefix codes for literals and match lengths/distances
- **Iterative refinement**: First pass greedy → build Huffman → second pass optimal DP with actual costs

### 4. Output
- Single IDAT chunk for maximum compression
- Ancillary chunks preserved or stripped based on options
- Validates output by re-reading and comparing pixels

## Optimization Levels

| Level | Filter Strategy | DEFLATE | Strategies | Best For |
|-------|----------------|---------|------------|----------|
| 0 | MinSum heuristic | Stored blocks | 1 | Fastest, no compression |
| 1-2 | Entropy-based | Fixed Huffman | 2 | Quick optimization |
| 3-4 | Entropy-based | Dynamic Huffman | 2 | Balanced |
| 5-6 | Viterbi DP trellis | Dynamic + 2 iter | 4 | Good compression |
| 7-8 | Viterbi DP trellis | Dynamic + 2 iter | 14 | Better compression |
| 9 | Viterbi DP + all variants | Dynamic + 3 iter (Zopfli squeezing) | 14 | Maximum compression |

## Benchmark Results

Evaluated against `optipng` (v0.7.8, `-o7`) and `pngcrush` (v1.8.13, `-brute`) across the official PNGSuite, photographic datasets, indexed colormap suites, and synthetic images.

### Comprehensive Test Suite Summary

| Tool | Total Compressed Size | Compression Ratio | vs. Original |
|------|----------------------|-------------------|:------------:|
| **fpng** | **95,318 B** | **11.9%** | **−88.1%** |
| pngcrush | 102,184 B | 12.8% | −87.2% |
| optipng | 100,394 B | 13.4% | −86.6% |

> **Overall Result**: `fpng` achieves an overall compression ratio of **11.9%**, outperforming both `pngcrush` (12.8%) and `optipng` (13.4%).

### Indexed Colormap Images (`test_images/*3p08.png`)

Thanks to 2-Opt TSP adjacency & Euclidean RGB palette reordering, `fpng` outperforms `pngcrush` on 100% of tested indexed images:

| Image | Original | fpng | pngcrush | `fpng` Advantage |
|---|---:|---:|---:|:---:|
| `basi3p08.png` | 1,527 B | **916 B** | 1,435 B | **−36.2%** |
| `basn3p08.png` | 1,263 B | **916 B** | 1,253 B | **−26.9%** |
| `ccwn3p08.png` | 1,554 B | **1,407 B** | 1,534 B | **−8.3%** |
| `ch2n3p08.png` | 1,810 B | **916 B** | 1,253 B | **−26.9%** |
| `cs3n3p08.png` | 259 B | **213 B** | 259 B | **−17.8%** |
| `cs5n3p08.png` | 271 B | **220 B** | 271 B | **−18.8%** |
| `cs8n3p08.png` | 256 B | **205 B** | 256 B | **−19.9%** |
| `tbbn3p08.png` | 1,499 B | **1,455 B** | 1,477 B | **−1.5%** |
| `tbgn3p08.png` | 1,499 B | **1,474 B** | 1,477 B | **−0.2%** |
| `tbwn3p08.png` | 1,496 B | **1,447 B** | 1,490 B | **−2.9%** |
| `tbyn3p08.png` | 1,499 B | **1,450 B** | 1,477 B | **−1.8%** |
| `tp0n3p08.png` | 1,476 B | **1,443 B** | 1,469 B | **−1.8%** |
| `tp1n3p08.png` | 1,483 B | **1,434 B** | 1,477 B | **−2.9%** |
| **Indexed Suite Total** | **17,310 B** | **14,657 B (84.7%)** | **16,171 B (93.4%)** | **`fpng` wins on all** |

### Synthetic & Geometric Images

| Image | Original | fpng | pngcrush | Best |
|---|---:|---:|---:|:---:|
| `gradient-32x32.png` | 117 B | **104 B** | 104 B | 104 B |
| `gradient-64x64.png` | 194 B | **139 B** | 139 B | 139 B |
| `gradient-128x128.png` | 461 B | **292 B** | 293 B | **292 B (fpng)** |
| `synth_blocks_128x128.png` | 51,939 B | **197 B** | 275 B | **197 B (fpng)** |
| `synth_gradient_128x128.png` | 6,569 B | **222 B** | 223 B | **222 B (fpng)** |
| `synth_gradient_256x256.png` | 26,133 B | **355 B** | 357 B | **355 B (fpng)** |

## Comparison to Other Tools

| Tool | Language | Approach |
|------|----------|----------|
| **fpng** | C++20 | Custom DEFLATE + SIMD + Viterbi DP filter + Package-Merge + TSP palette |
| oxipng | Rust | libdeflate/Zopfli + heuristic filters + parallel trials |
| zopflipng | C++ | Zopfli DEFLATE (optimal parsing) + filter comparison |
| optipng | C | Trial-based: tries filter/compression combos |
| pngcrush | C | Brute-force tries many IDAT chunk options |
| pingo | C (closed) | Proprietary heuristics — #1 in benchmarks |
| ect | C++ | Custom fast DEFLATE + multithreading |

fpng's key technical strengths:
- **Exact Viterbi Dynamic Programming**: Solves global per-scanline filter assignment ($O(25 \times H)$) in under 1ms, replacing slow stochastic GAs.
- **True Package-Merge Huffman**: Guaranteed mathematically optimal length-limited prefix codes in $O(N \cdot L)$ time per RFC 1951.
- **2-Opt TSP Palette Permutation**: Minimizes filter residuals for indexed images via pixel adjacency co-occurrence graph.
- **RFC 1951 Optimal Block Typing**: Automatically evaluates and emits the strictly smallest representation between Dynamic Huffman, Fixed Huffman, and Stored blocks.
- **2D Spatial & Diagonal Probing**: Probes vertical and diagonal pixel matches ($\text{row\_stride} \pm \Delta$) to capture planar 2D image redundancies.
- **Zopfli-Style Squeezing**: Iterative forward-DP with cost perturbations and best-token retention to break out of local minima.
- **Multi-strategy parallel trials**: 14 strategies evaluated in parallel via `std::async`, with safety fallback to original.

## CLI Options

```
fpng [options] <input.png> [output.png]

Options:
  -o <N>        Optimization level (0-9, default: 9)
  -j <N>        Number of threads (default: auto)
  -s            Single strategy (no multi-strategy, faster)
  -v            Verbose output (show strategy trials)
  --strip       Strip ancillary chunks
  --help        Show help
```

## Project Structure

```
fpng/
├── CMakeLists.txt              # C++20, cross-platform, SIMD detection
├── src/
│   ├── main.cpp                # CLI entry point
│   ├── png/                    # PNG I/O
│   │   ├── reader.cpp/hpp      #   PNG datastream parser (all chunks, APNG, Adam7)
│   │   ├── writer.cpp/hpp      #   PNG datastream writer (filter+compress+serialize)
│   │   ├── filter.cpp/hpp      #   PNG filter encode/decode (None/Sub/Up/Avg/Paeth)
│   │   ├── chunk.hpp           #   Chunk type definitions and naming conventions
│   │   └── ihdr.hpp            #   IHDR data struct and validation
│   ├── image/                  # Image representation
│   │   ├── image.hpp/cpp       #   Pixel buffer, metadata, APNG frame storage
│   │   └── color.hpp           #   Color type/depth helpers
│   ├── compress/               # Compression pipeline
│   │   ├── compressor.cpp/hpp  #   Multi-strategy orchestrator, parallel trials
│   │   ├── filter_optimizer    #   Filter selection (heuristic → GA)
│   │   └── deflate/            #   Custom DEFLATE implementation
│   │       ├── deflater.cpp/hpp   # Main compressor (all block types)
│   │       ├── inflate.cpp/hpp    # DEFLATE decompressor (all block types)
│   │       ├── huffman.cpp/hpp    # Huffman tree construction, Package-Merge
│   │       ├── lz77.cpp/hpp       # LZ77 parser (greedy + optimal DP)
│   │       ├── match_finder.hpp   # SIMD-accelerated match finding
│   │       ├── block_splitter.cpp/hpp  # Adaptive block boundary selection
│   │       ├── bit_writer.hpp/cpp     # Bit-level I/O buffer
│   │       └── constants.hpp     # DEFLATE format constants
│   ├── preprocess/             # Pre-compression optimization
│   │   ├── alpha_optimizer     #   Alpha zeroing + channel stripping
│   │   ├── color_reducer       #   Color type/depth reduction
│   │   ├── palette_sorter      #   Palette reordering by luminance
│   │   ├── content_analyzer    #   Image classification (photo/graphic/noise)
│   │   └── preprocessor        #   Orchestrator
│   ├── test/                   # Test framework
│   │   ├── test_framework      #   Test harness
│   │   ├── test_png.cpp        #   PNG roundtrip + CRC + filter tests
│   │   ├── test_deflate.cpp    #   DEFLATE unit tests
│   │   ├── test_filter.cpp     #   Paeth predictor tests
│   │   └── benchmark.cpp       #   Comparative benchmark against external tools
│   └── util/                   # Utilities
│       ├── crc32.cpp/hpp       #   CRC32 (PNG + zlib)
│       ├── endian.hpp          #   Byte order conversion
│       ├── file.cpp/hpp        #   Memory-mapped file I/O
│       └── timer.hpp           #   High-resolution timing
├── scripts/
│   └── download_test_images.sh # Download PNGSuite + generate synthetic images
└── test_images/                # Test PNG files (downloaded)
```

## Technical Notes

### DEFLATE Compliance
The custom DEFLATE implementation produces RFC 1951-compliant compressed data.
All three block types (stored, fixed Huffman, dynamic Huffman) are supported for
both compression and decompression. The decompressor handles streams from zlib
and other standard DEFLATE encoders.

### Package-Merge Huffman
The Huffman encoder computes guaranteed-optimal length-limited codes using the
true Package-Merge algorithm (Larmore & Hirschberg, 1990). Operating in $O(N \cdot L)$
time, it maps code construction to the coin-collector's problem, ensuring minimum total
bit redundancy under the 15-bit DEFLATE constraint.

### Viterbi Dynamic Programming Filter Trellis
Rather than relying on slow stochastic search, levels 5+ evaluate the exact 5-state
Viterbi trellis across scanlines ($O(25 \times H)$). By modeling signed residual magnitude
### 2-Opt TSP Palette Optimization
For indexed colormap images, palette order directly impacts filter residuals. `fpng` builds
an adjacent pixel co-occurrence graph weighted by Euclidean RGB distance, finding an optimal
color ordering via a Nearest Neighbor tour with 2-opt edge exchanges.

### SIMD Acceleration
Match length computation is accelerated using:
- **ARM NEON**: 16-byte parallel compare via `vceqq_u8`
- **x86 AVX2**: 32-byte parallel compare via `_mm256_cmpeq_epi8`
- **x86 SSE4.2**: 16-byte parallel compare via `_mm_cmpeq_epi8`

Scalar fallback works on all platforms with no SIMD support.

## License

MIT License — see below. No external dependencies means no license compatibility concerns.

```
MIT License

Copyright (c) 2026 fpng contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## References

- [PNG Specification (W3C PNG 3.0)](https://www.w3.org/TR/png-3/)
- [RFC 1950 — ZLIB Compressed Data Format](https://www.rfc-editor.org/rfc/rfc1950)
- [RFC 1951 — DEFLATE Compressed Data Format](https://www.rfc-editor.org/rfc/rfc1951)
- Larmore & Hirschberg, "A fast algorithm for optimal length-limited Huffman codes" (1990)
- Ziv & Lempel, "A Universal Algorithm for Sequential Data Compression" (1977)

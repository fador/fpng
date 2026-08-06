#include "png/reader.hpp"
#include "png/writer.hpp"
#include "compress/compressor.hpp"
#include "util/timer.hpp"
#include "util/file.hpp"

#include <iostream>
#include <iomanip>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)
#define FPNG_OS_WINDOWS
#endif

// Shared temp directory for benchmark outputs.
static fs::path bench_dir() {
#ifdef FPNG_OS_WINDOWS
    static fs::path p = fs::temp_directory_path() / "fpng_bench";
#else
    static fs::path p = fs::path("/tmp/fpng_bench");
#endif
    return p;
}

// Cross-platform shell helpers.
static std::string null_dev() {
#ifdef FPNG_OS_WINDOWS
    return "nul";
#else
    return "/dev/null";
#endif
}

static std::string which_cmd(const std::string& cmd) {
#ifdef FPNG_OS_WINDOWS
    return "where " + cmd + " > " + null_dev() + " 2>&1";
#else
    return "which " + cmd + " > " + null_dev() + " 2>&1";
#endif
}

static std::string exe_suffix() {
#ifdef FPNG_OS_WINDOWS
    return ".exe";
#else
    return "";
#endif
}

// Resolve the fpng executable path, relative to this benchmark's own location
// so it works regardless of the build directory layout.
static std::string get_fpng_path() {
    static std::string cached;
    if (!cached.empty()) return cached;
    fs::path self;
#ifdef FPNG_OS_WINDOWS
    {
        char buf[MAX_PATH];
        GetModuleFileNameA(NULL, buf, MAX_PATH);
        self = fs::path(buf).parent_path();
    }
#else
    {
        std::string link = "/proc/self/exe";
        fs::path p(link);
        std::error_code ec;
        if (fs::exists(p, ec)) self = fs::read_symlink(p, ec).parent_path();
        else self = fs::current_path();
    }
#endif
    cached = (self / ("fpng" + exe_suffix())).string();
    return cached;
}

struct ToolResult {
    std::string tool;
    size_t original_size = 0;
    size_t compressed_size = 0;
    double time_seconds = 0;
    bool success = false;
};

struct BenchmarkResult {
    std::string filename;
    size_t original_size = 0;
    std::vector<ToolResult> tools;
};

// Check if a command exists
static bool command_exists(const std::string& cmd) {
    return std::system(which_cmd(cmd).c_str()) == 0;
}

// Run a tool on a file
static ToolResult run_tool(const std::string& tool, const std::string& input,
                            const std::string& output) {
    ToolResult r;
    r.tool = tool;

    std::string dev_null = null_dev();
    std::string fpng_exe = get_fpng_path();

    // Build command
    std::string cmd;
    if (tool == "fpng") {
        cmd = "\"" + fpng_exe + "\" -s -v \"" + input + "\" \"" + output + "\" 2>&1";
    } else if (tool == "optipng") {
        cmd = "optipng -o7 -out \"" + output + "\" \"" + input + "\" 2> " + dev_null;
    } else if (tool == "oxipng") {
        cmd = "oxipng -o6 --out \"" + output + "\" \"" + input + "\" 2> " + dev_null;
    } else if (tool == "zopflipng") {
        cmd = "zopflipng --iterations=15 \"" + input + "\" \"" + output + "\" 2> " + dev_null;
    } else if (tool == "advpng") {
        // advpng modifies in-place
#ifdef FPNG_OS_WINDOWS
        std::system(("copy /Y \"" + input + "\" \"" + output + "\" > " + dev_null).c_str());
#else
        std::string cp_cmd = "cp " + input + " " + output;
        std::system(cp_cmd.c_str());
#endif
        cmd = "advpng -z4 \"" + output + "\" 2> " + dev_null;
    } else if (tool == "pngcrush") {
        cmd = "pngcrush -brute \"" + input + "\" \"" + output + "\" 2> " + dev_null;
    } else if (tool == "ect") {
#ifdef FPNG_OS_WINDOWS
        cmd = "ect -9 -strip \"" + input + "\" 2> " + dev_null + " && copy /Y \"" + input + "\" \"" + output + "\" > " + dev_null;
#else
        cmd = "ect -9 -strip " + input + " 2>/dev/null && cp " + input + " " + output;
#endif
    } else {
        r.success = false;
        return r;
    }

    fpng::Timer t;
    // Wrap the command in quotes for std::system: on Windows, cmd.exe strips
    // the outer quotes, so a command whose first token is a quoted path is
    // otherwise misparsed (error 123).
#ifdef FPNG_OS_WINDOWS
    int ret = std::system(("\"" + cmd + "\"").c_str());
#else
    int ret = std::system(cmd.c_str());
#endif
    r.time_seconds = t.elapsed_seconds();

    if (ret == 0 && fs::exists(output)) {
        r.compressed_size = fs::file_size(output);
        r.success = true;
    } else {
        r.success = false;
    }

    return r;
}

// Generate a small test image
static std::vector<uint8_t> make_test_image(int w, int h, int pattern) {
    // Create a simple RGB PNG using fpng's library
    fpng::Image img;
    img.width = w;
    img.height = h;
    img.bit_depth = 8;
    img.color_type = 6; // RGBA
    img.interlaced = false;

    size_t row_size = img.raw_scanline_size();
    img.pixels.resize(row_size * h);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t off = y * row_size + x * 4;
            if (pattern == 0) {
                // Gradient
                img.pixels[off + 0] = static_cast<uint8_t>(x * 255 / w);
                img.pixels[off + 1] = static_cast<uint8_t>(y * 255 / h);
                img.pixels[off + 2] = static_cast<uint8_t>((x + y) * 128 / (w + h));
                img.pixels[off + 3] = 255;
            } else if (pattern == 1) {
                // Solid color blocks
                img.pixels[off + 0] = static_cast<uint8_t>((x / (w/4)) * 85);
                img.pixels[off + 1] = static_cast<uint8_t>((y / (h/4)) * 85);
                img.pixels[off + 2] = 128;
                img.pixels[off + 3] = 255;
            } else {
                // Noise-like
                img.pixels[off + 0] = static_cast<uint8_t>((x * 17 + y * 13) % 256);
                img.pixels[off + 1] = static_cast<uint8_t>((x * 31 + y * 7) % 256);
                img.pixels[off + 2] = static_cast<uint8_t>((x * 11 + y * 23) % 256);
                img.pixels[off + 3] = 255;
            }
        }
    }

    fpng::PNGWriter writer;
    return writer.write(img);
}

static void print_header() {
    std::cout << "\n";
    std::cout << std::setw(30) << std::left << "File"
              << std::setw(10) << std::right << "Orig"
              << std::setw(12) << "fpng"
              << std::setw(12) << "optipng"
              << std::setw(12) << "oxipng"
              << std::setw(12) << "zopflipng"
              << std::setw(10) << "Best"
              << "\n";
    std::cout << std::string(98, '-') << "\n";
}

int main(int argc, char** argv) {
    std::cout << "fpng Benchmark\n";
    std::cout << "==============\n";

    (void)argc;
    (void)argv;

    // Check available tools
    std::vector<std::string> tools = {"fpng"};
    std::cout << "Available tools: fpng";

    if (command_exists("optipng")) { tools.push_back("optipng"); std::cout << ", optipng"; }
    if (command_exists("oxipng")) { tools.push_back("oxipng"); std::cout << ", oxipng"; }
    if (command_exists("zopflipng")) { tools.push_back("zopflipng"); std::cout << ", zopflipng"; }
    if (command_exists("advpng")) { tools.push_back("advpng"); std::cout << ", advpng"; }
    if (command_exists("pngcrush")) { tools.push_back("pngcrush"); std::cout << ", pngcrush"; }
    if (command_exists("ect")) { tools.push_back("ect"); std::cout << ", ect"; }
    std::cout << "\n\n";

    if (tools.empty()) {
        std::cout << "No compression tools found.\n";
        std::cout << "Install: apt install optipng oxipng zopflipng advancecomp pngcrush\n";
        return 0;
    }

    // Generate test images
    fs::create_directories(bench_dir());
    std::vector<std::string> images;

    // Generate test images of various sizes and patterns
    struct { int w, h, pattern; const char* name; } specs[] = {
        {32, 32, 0, "gradient-32x32"},
        {64, 64, 0, "gradient-64x64"},
        {128, 128, 0, "gradient-128x128"},
        {32, 32, 1, "blocks-32x32"},
        {64, 64, 1, "blocks-64x64"},
        {32, 32, 2, "noise-32x32"},
        {64, 64, 2, "noise-64x64"},
    };

    for (auto& s : specs) {
        auto data = make_test_image(s.w, s.h, s.pattern);
        std::string path = (bench_dir() / (std::string(s.name) + ".png")).string();
        fpng::write_file(path, data);
        images.push_back(path);
        std::cout << "Generated: " << s.name << " (" << data.size() << " bytes)\n";
    }

    // Also add any PNG files provided on command line
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (fs::exists(arg) && fs::path(arg).extension() == ".png") {
            images.push_back(arg);
        }
    }

    // Run benchmarks
    std::vector<BenchmarkResult> results;
    print_header();

    for (auto& img_path : images) {
        BenchmarkResult br;
        br.filename = fs::path(img_path).filename().string();
        br.original_size = fs::file_size(img_path);

        for (auto& tool : tools) {
            std::string out_path = (bench_dir() /
                                    (fs::path(img_path).filename().stem().string() +
                                     "_" + tool + ".png")).string();

            auto tr = run_tool(tool, img_path, out_path);
            tr.original_size = br.original_size;
            br.tools.push_back(tr);
        }

        results.push_back(br);

        // Print row
        std::cout << std::setw(30) << std::left << br.filename.substr(0, 29)
                  << std::setw(10) << std::right << br.original_size;

        size_t best = br.original_size;
        for (auto& t : br.tools) {
            if (t.success && t.compressed_size > 0) {
                std::cout << std::setw(12) << t.compressed_size;
                if (t.compressed_size < best) best = t.compressed_size;
            } else {
                std::cout << std::setw(12) << "N/A";
            }
        }
        std::cout << std::setw(10) << best << "\n";
    }

    // Summary
    std::cout << "\n";
    std::cout << std::string(98, '-') << "\n";
    std::cout << std::setw(30) << std::left << "Total"
              << std::setw(10) << std::right
              << std::accumulate(results.begin(), results.end(), size_t(0),
                  [](size_t s, const BenchmarkResult& r) { return s + r.original_size; });

    for (size_t ti = 0; ti < tools.size(); ++ti) {
        size_t total = 0;
        bool any = false;
        for (auto& r : results) {
            if (ti < r.tools.size() && r.tools[ti].success) {
                total += r.tools[ti].compressed_size;
                any = true;
            }
        }
        if (any) std::cout << std::setw(12) << total;
        else std::cout << std::setw(12) << "N/A";
    }
    std::cout << "\n";

    // Average savings
    std::cout << "\nCompression Ratio (lower is better):\n";
    for (auto& tool : tools) {
        double total_orig = 0, total_comp = 0;
        for (auto& r : results) {
            for (auto& t : r.tools) {
                if (t.tool == tool && t.success) {
                    total_orig += t.original_size;
                    total_comp += t.compressed_size;
                }
            }
        }
        if (total_orig > 0) {
            double ratio = 100.0 * total_comp / total_orig;
            std::cout << "  " << std::setw(10) << std::left << tool
                      << std::fixed << std::setprecision(1) << ratio << "%\n";
        }
    }

    std::cout << "\nDone.\n";
    return 0;
}

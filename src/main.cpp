#include "png/reader.hpp"
#include "compress/compressor.hpp"
#include "util/timer.hpp"
#include "util/file.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <system_error>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] <input.png> [output.png]\n";
    std::cout << "  fpng - extremely efficient PNG compressor (C++20)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o <N>        Optimization level (0-9, default: 9)\n";
    std::cout << "  -j <N>        Number of threads (default: auto)\n";
    std::cout << "  -s            Single strategy (no multi-strategy)\n";
    std::cout << "  -v            Verbose output\n";
    std::cout << "  --strip       Strip ancillary chunks\n";
    std::cout << "  --help        Show this help\n";
    std::cout << "\nExit status: 0 = written or kept original, 1 = usage error,\n"
                 "             2 = input could not be read, 3 = output could not be written.\n";
}

// Printable UTF-8 form of a path; never throws (path::string() can throw on
// Windows for characters outside the ANSI code page).
std::string display(const fs::path& p) {
    try {
        auto u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    } catch (...) {
        return "<path>";
    }
}

bool parse_int(const std::string& s, int lo, int hi, int& out) {
    try {
        size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size() || v < lo || v > hi) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Write `data` next to `target` and atomically move it into place, so a crash
// or full disk never leaves a truncated file where the original used to be.
bool atomic_write(const fs::path& target, const std::vector<uint8_t>& data, std::string& err) {
    fs::path tmp = target;
    tmp += ".fpng-tmp";
    if (!fpng::write_file(tmp, data)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        err = "cannot write temporary file";
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);  // replaces existing target (also on Windows)
    if (ec) {
        // Fallback for filesystems where rename-over fails.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        std::error_code ec2;
        fs::remove(tmp, ec2);
        if (ec) {
            err = ec.message();
            return false;
        }
    }
    return true;
}

int run(const std::vector<fs::path>& args_path, const std::vector<std::string>& args, const char* prog) {
    fpng::CompressOptions opts;
    fs::path input_path, output_path;
    bool have_in = false, have_out = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(prog);
            return 0;
        } else if (arg == "-v") {
            opts.verbose = true;
        } else if (arg == "-s") {
            opts.multi_strategy = false;
        } else if (arg == "--strip") {
            opts.strip_ancillary = true;
        } else if (arg == "-o" || arg == "-j") {
            int v = 0;
            if (i + 1 >= args.size() ||
                !parse_int(args[i + 1], arg == "-o" ? 0 : 0, arg == "-o" ? 9 : 1024, v)) {
                std::cerr << "Error: " << arg << " needs a number ("
                          << (arg == "-o" ? "0-9" : "0-1024") << ")\n";
                return 1;
            }
            (arg == "-o" ? opts.level : opts.num_threads) = v;
            ++i;
        } else if (arg.size() > 1 && arg[0] == '-' && arg != "-") {
            std::cerr << "Error: unknown option " << arg << "\n";
            return 1;
        } else if (!have_in) {
            input_path = args_path[i];
            have_in = true;
        } else if (!have_out) {
            output_path = args_path[i];
            have_out = true;
        } else {
            std::cerr << "Error: too many arguments\n";
            return 1;
        }
    }

    if (!have_in) {
        print_usage(prog);
        return 1;
    }
    if (!have_out) output_path = input_path;

    fpng::Timer total_timer;

    std::vector<uint8_t> original = fpng::read_file(input_path);
    if (original.empty()) {
        std::cerr << "Error: cannot read " << display(input_path) << "\n";
        return 2;
    }

    fpng::PNGReader reader;
    auto img_opt = reader.read(original);
    if (!img_opt) {
        std::cerr << "Error reading PNG " << display(input_path) << ": " << reader.error() << "\n";
        return 2;
    }
    for (const auto& w : reader.warnings())
        std::cerr << "warning: " << display(input_path) << ": " << w << "\n";

    auto& img = *img_opt;
    double read_time = total_timer.elapsed_seconds();

    if (opts.verbose) {
        std::cout << "Read: " << img.width << "x" << img.height
                  << " color_type=" << static_cast<int>(img.color_type)
                  << " bit_depth=" << static_cast<int>(img.bit_depth)
                  << " interlaced=" << (img.interlaced ? "yes" : "no")
                  << " animated=" << (img.is_animated ? "yes" : "no")
                  << " (" << read_time << "s)\n";
        if (img.is_animated) std::cout << "  Frames: " << img.frames.size() << "\n";
    }

    fpng::Timer comp_timer;
    fpng::CompressResult result;
    bool compressed_ok = false;
    try {
        result = fpng::compress(img, opts);  // verified (decoded + compared) output
        compressed_ok = !result.data.empty();
    } catch (const std::bad_alloc&) {
        std::cerr << "warning: out of memory while compressing; keeping original\n";
    } catch (const std::exception& e) {
        std::cerr << "warning: compression failed (" << e.what() << "); keeping original\n";
    }
    double comp_time = comp_timer.elapsed_seconds();

    const size_t orig_size = original.size();
    const bool use_new = compressed_ok && result.data.size() < orig_size;
    const bool same_file = [&] {
        std::error_code ec;
        return fs::exists(output_path, ec) && fs::equivalent(input_path, output_path, ec);
    }();

    std::string err;
    if (use_new) {
        if (!atomic_write(output_path, result.data, err)) {
            std::cerr << "Error: cannot write " << display(output_path) << ": " << err << "\n";
            return 3;
        }
    } else if (!same_file) {
        // Output path requested but nothing better found: emit the original bytes.
        if (!atomic_write(output_path, original, err)) {
            std::cerr << "Error: cannot write " << display(output_path) << ": " << err << "\n";
            return 3;
        }
    }

    double total_time = total_timer.elapsed_seconds();
    if (opts.verbose && compressed_ok) {
        std::cout << "Strategy: " << result.strategy_name << "\n";
        std::cout << "Compression: " << comp_time << "s\n";
    }
    if (use_new) {
        double ratio = 100.0 * (1.0 - static_cast<double>(result.data.size()) / orig_size);
        std::cout << "Wrote: " << display(output_path) << " (" << result.data.size()
                  << " bytes, " << ratio << "% saved) in " << total_time << "s\n";
    } else {
        std::cout << "Kept: " << display(output_path) << " (" << orig_size << " bytes, "
                  << (compressed_ok ? "already optimal" : "original preserved") << ") in "
                  << total_time << "s\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::vector<fs::path> paths;
#ifdef _WIN32
    // Use the UTF-16 command line so non-ASCII file names (e.g. "Käyttäjät")
    // survive; narrow argv is lossy outside the ANSI code page.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i) {
            fs::path p(wargv[i]);
            paths.push_back(p);
            args.push_back(display(p));
        }
        LocalFree(wargv);
    } else
#endif
    {
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
            paths.emplace_back(argv[i]);
        }
    }
    if (args.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    try {
        return run(paths, args, argv[0]);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 3;
    } catch (...) {
        std::cerr << "Fatal error\n";
        return 3;
    }
}

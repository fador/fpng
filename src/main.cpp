#include "png/reader.hpp"
#include "compress/compressor.hpp"
#include "util/timer.hpp"
#include "util/file.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <thread>

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
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    fpng::CompressOptions opts;
    std::string input_path;
    std::string output_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v") {
            opts.verbose = true;
        } else if (arg == "-s") {
            opts.multi_strategy = false;
        } else if (arg == "--strip") {
            opts.strip_ancillary = true;
        } else if (arg == "-o" && i + 1 < argc) {
            opts.level = std::atoi(argv[++i]);
        } else if (arg == "-j" && i + 1 < argc) {
            opts.num_threads = std::atoi(argv[++i]);
        } else if (input_path.empty()) {
            input_path = arg;
        } else if (output_path.empty()) {
            output_path = arg;
        }
    }

    if (input_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (output_path.empty()) output_path = input_path;

    fpng::Timer total_timer;

    // Read
    fpng::PNGReader reader;
    auto img_opt = reader.read(input_path);
    if (!img_opt) {
        std::cerr << "Error reading PNG: " << reader.error() << "\n";
        return 1;
    }

    auto& img = *img_opt;
    double read_time = total_timer.elapsed_seconds();

    if (opts.verbose) {
        std::cout << "Read: " << img.width << "x" << img.height
                  << " color_type=" << static_cast<int>(img.color_type)
                  << " bit_depth=" << static_cast<int>(img.bit_depth)
                  << " animated=" << (img.is_animated ? "yes" : "no")
                  << " (" << read_time << "s)\n";
        if (img.is_animated) {
            std::cout << "  Frames: " << img.frames.size() << "\n";
        }
    }

    // Compress
    fpng::Timer comp_timer;
    auto result = fpng::compress(img, opts);
    double comp_time = comp_timer.elapsed_seconds();

    // Write
    fpng::write_file(output_path, result.data);

    double total_time = total_timer.elapsed_seconds();

    if (opts.verbose) {
        std::cout << "Strategy: " << result.strategy_name << "\n";
        std::cout << "Compression: " << comp_time << "s\n";
    }

    size_t orig_size = result.original_size;
    size_t out_size = result.data.size();
    if (orig_size > 0) {
        double ratio = 100.0 * (1.0 - static_cast<double>(out_size) / orig_size);
        std::cout << "Wrote: " << output_path
                  << " (" << out_size << " bytes, "
                  << ratio << "% saved)"
                  << " in " << total_time << "s\n";
    } else {
        std::cout << "Wrote: " << output_path
                  << " (" << out_size << " bytes)"
                  << " in " << total_time << "s\n";
    }

    return 0;
}

#include "png/reader.hpp"
#include "png/writer.hpp"
#include "util/timer.hpp"
#include "util/file.hpp"

#include <iostream>
#include <filesystem>
#include <string>

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <input.png> [output.png]\n";
    std::cout << "  fpng - extremely efficient PNG compressor (C++20)\n";
    std::cout << "  If output is omitted, overwrites input.\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path = argv[1];
    std::string output_path = (argc >= 3) ? argv[2] : input_path;

    fpng::Timer total_timer;

    fpng::PNGReader reader;
    auto img_opt = reader.read(input_path);
    if (!img_opt) {
        std::cerr << "Error reading PNG: " << reader.error() << "\n";
        return 1;
    }

    auto& img = *img_opt;
    double read_time = total_timer.elapsed_seconds();
    std::cout << "Read: " << img.width << "x" << img.height
              << " color_type=" << static_cast<int>(img.color_type)
              << " bit_depth=" << static_cast<int>(img.bit_depth)
              << " animated=" << (img.is_animated ? "yes" : "no")
              << " (" << read_time << "s)\n";

    if (img.is_animated) {
        std::cout << "  Frames: " << img.frames.size() << "\n";
        for (size_t i = 0; i < img.frames.size(); ++i) {
            auto& f = img.frames[i];
            std::cout << "    Frame " << i << ": " << f.width << "x" << f.height
                      << " at (" << f.x_offset << "," << f.y_offset << ")"
                      << " delay=" << f.delay_num << "/" << f.delay_den << "\n";
        }
    }

    fpng::PNGWriter writer;
    auto out = writer.write(img);
    fpng::write_file(output_path, out);

    double total_time = total_timer.elapsed_seconds();
    std::cout << "Wrote: " << output_path << " (" << out.size() << " bytes)"
              << " in " << total_time << "s\n";

    return 0;
}

#include "compress/compressor.hpp"
#include "compress/deflate/deflater.hpp"
#include "png/writer.hpp"

namespace fpng {
CompressResult compress(const Image& img, const CompressOptions& opts) {
    (void)opts;
    PNGWriter writer;
    auto out = writer.write(img);
    return {std::move(out), 0.0, 0};
}
} // namespace fpng

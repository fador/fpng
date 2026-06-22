#include "png/filter.hpp"

#include <cstring>

namespace fpng {

void filter_scanline(FilterType type,
                     const uint8_t* src, uint8_t* dst,
                     size_t pixel_stride, size_t byte_width,
                     const uint8_t* prev_scanline) {
    dst[0] = static_cast<uint8_t>(type);

    auto* out = dst + 1;

    switch (type) {
    case FilterType::None:
        std::memcpy(out, src, byte_width);
        return;

    case FilterType::Sub: {
        std::memcpy(out, src, pixel_stride);
        for (size_t i = pixel_stride; i < byte_width; ++i)
            out[i] = static_cast<uint8_t>(src[i] - src[i - pixel_stride]);
        return;
    }

    case FilterType::Up: {
        if (!prev_scanline) {
            std::memcpy(out, src, byte_width);
        } else {
            for (size_t i = 0; i < byte_width; ++i)
                out[i] = static_cast<uint8_t>(src[i] - prev_scanline[i]);
        }
        return;
    }

    case FilterType::Average: {
        if (!prev_scanline) {
            std::memcpy(out, src, pixel_stride);
            for (size_t i = pixel_stride; i < byte_width; ++i)
                out[i] = static_cast<uint8_t>(src[i] - (src[i - pixel_stride] >> 1));
        } else {
            for (size_t i = 0; i < pixel_stride; ++i)
                out[i] = static_cast<uint8_t>(src[i] - (prev_scanline[i] >> 1));
            for (size_t i = pixel_stride; i < byte_width; ++i) {
                int avg = (static_cast<int>(src[i - pixel_stride]) +
                           static_cast<int>(prev_scanline[i])) >> 1;
                out[i] = static_cast<uint8_t>(src[i] - avg);
            }
        }
        return;
    }

    case FilterType::Paeth: {
        for (size_t i = 0; i < byte_width; ++i) {
            int a = (i >= pixel_stride) ? src[i - pixel_stride] : 0;
            int b = prev_scanline ? prev_scanline[i] : 0;
            int c = 0;
            if (i >= pixel_stride && prev_scanline)
                c = prev_scanline[i - pixel_stride];
            out[i] = static_cast<uint8_t>(src[i] - paeth_predictor(a, b, c));
        }
        return;
    }
    }
}

void unfilter_scanline(FilterType type,
                       const uint8_t* filtered, uint8_t* recon,
                       size_t pixel_stride, size_t byte_width,
                       const uint8_t* prev_scanline) {
    switch (type) {
    case FilterType::None:
        std::memcpy(recon, filtered, byte_width);
        return;

    case FilterType::Sub:
        for (size_t i = 0; i < byte_width; ++i) {
            uint8_t left = (i >= pixel_stride) ? recon[i - pixel_stride] : 0;
            recon[i] = static_cast<uint8_t>(filtered[i] + left);
        }
        return;

    case FilterType::Up:
        for (size_t i = 0; i < byte_width; ++i) {
            uint8_t up = prev_scanline ? prev_scanline[i] : 0;
            recon[i] = static_cast<uint8_t>(filtered[i] + up);
        }
        return;

    case FilterType::Average:
        for (size_t i = 0; i < byte_width; ++i) {
            uint8_t left = (i >= pixel_stride) ? recon[i - pixel_stride] : 0;
            uint8_t up   = prev_scanline ? prev_scanline[i] : 0;
            recon[i] = static_cast<uint8_t>(filtered[i] + ((int(left) + int(up)) >> 1));
        }
        return;

    case FilterType::Paeth:
        for (size_t i = 0; i < byte_width; ++i) {
            int a = (i >= pixel_stride) ? recon[i - pixel_stride] : 0;
            int b = prev_scanline ? prev_scanline[i] : 0;
            int c = (i >= pixel_stride && prev_scanline)
                        ? prev_scanline[i - pixel_stride] : 0;
            recon[i] = static_cast<uint8_t>(filtered[i] + paeth_predictor(a, b, c));
        }
        return;
    }
}

} // namespace fpng

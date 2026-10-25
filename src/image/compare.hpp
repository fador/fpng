#pragma once

#include "image/image.hpp"

#include <string>

namespace fpng {

// True when `a` and `b` display identically: every pixel expands to the same
// 16-bit RGBA value (palette and tRNS applied, samples scaled exactly to 16
// bits). The colour of fully transparent pixels is ignored. For animated
// images the frames (count, geometry, timing, disposal and pixels) must also
// match. On mismatch, `why` (if given) describes the first difference.
bool images_equivalent(const Image& a, const Image& b, std::string* why = nullptr);

} // namespace fpng

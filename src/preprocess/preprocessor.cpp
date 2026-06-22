#include "preprocess/preprocessor.hpp"
#include "preprocess/alpha_optimizer.hpp"
#include "preprocess/color_reducer.hpp"
#include "preprocess/palette_sorter.hpp"
#include "preprocess/content_analyzer.hpp"

namespace fpng {

void preprocess(Image& img) {
    // Step 1: Alpha optimization (zero transparent RGB)
    alpha_optimize(img);

    // Step 2: Color type/depth reduction
    reduce_colors(img);

    // Step 3: Palette sorting (for indexed images)
    sort_palette(img);

    // Content analysis stored for filter optimization
    // (analyze_content is called separately by the compressor)
}

} // namespace fpng

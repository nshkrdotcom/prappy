#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace compactrow {
struct Layout {
    std::vector<float> widths;
    size_t hidden = 0;
    float overflow = 0, used = 0;
};
template<class MeasureItem, class MeasureOverflow>
Layout plan(size_t count, float available, float gap, float minimum, float maximum, MeasureItem item, MeasureOverflow overflow) {
    // Measure only the visible prefix plus one candidate. Reserve the overflow
    // control before accepting a chip, so it never gets pushed outside the row.
    Layout result; result.hidden = count;
    if (!std::isfinite(available) || available <= 0) return result;
    gap = std::max(0.0f,gap); minimum = std::max(1.0f,minimum); maximum = std::max(minimum,maximum);
    for (size_t i = 0; i < count; ++i) {
        const auto spacing = result.widths.empty() ? 0.0f : gap;
        const auto reserve = i+1 < count ? gap + std::max(0.0f,overflow(count-i-1)) : 0.0f;
        const auto natural = item(i);
        const auto width = std::min(std::clamp(std::isfinite(natural) ? natural : minimum,minimum,maximum),available-result.used-spacing-reserve);
        if (width < minimum) break;
        result.widths.push_back(width); result.used += spacing+width; --result.hidden;
    }
    if (result.hidden) {
        const auto spacing = result.widths.empty() ? 0.0f : gap;
        result.overflow = std::min(std::max(0.0f,overflow(result.hidden)),std::max(0.0f,available-result.used-spacing));
        result.used += spacing+result.overflow;
    }
    return result;
}
}

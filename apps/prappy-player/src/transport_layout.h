#pragma once
#include <algorithm>
struct TransportLayout {
    static constexpr int Compact = 202, Maximum = 32768;
    static int height(int preferred, int windowHeight, bool expanded) {
        return std::clamp(preferred, Compact, std::max(Compact, windowHeight - (expanded ? 250 : 48)));
    }
    static int resized(int initial, float delta, int windowHeight, bool expanded) {
        return height(initial + static_cast<int>(delta), windowHeight, expanded);
    }
};

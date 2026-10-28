#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sampling {
struct WaveformRange {
    int64_t start=0,end=1;
    bool operator==(const WaveformRange&)const=default;
};
// A fractional view needs every intersecting sample: [floor(left), ceil(right)).
// Use this contract for both requests and envelope construction.
inline WaveformRange waveformRange(double left,double right,int64_t frames){
    frames=std::max<int64_t>(1,frames);
    if(!std::isfinite(left)||!std::isfinite(right))return {0,1};
    const auto first=static_cast<int64_t>(std::floor(std::clamp(left,0.,static_cast<double>(frames-1))));
    const auto end=static_cast<int64_t>(std::ceil(std::clamp(right,static_cast<double>(first+1),static_cast<double>(frames))));
    return {first,end};
}
}

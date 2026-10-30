#include "analysis_dsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace analysis {
namespace {
struct Biquad {
    double b0, b1, b2, a1, a2;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double process(double x) {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y; return y;
    }
};
struct Weighting {
    // ITU-R BS.1770-5 Annex 1, 48 kHz K-weighting coefficients.
    Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, .73248077421585};
    Biquad highpass{1, -2, 1, -1.99004745483398, .99007225036621};
    double process(double x) { return highpass.process(shelf.process(x)); }
};
Peak combine(const Peak& a, const Peak& b) {
    return {std::min(a.leftMin, b.leftMin), std::max(a.leftMax, b.leftMax),
        std::min(a.rightMin, b.rightMin), std::max(a.rightMax, b.rightMax)};
}

}
struct Accumulator::Impl {
    Result result;
    Weighting left, right;
    Peak peak;
    std::array<double, 4> energies{};
    double energy = 0;
    uint64_t hops = 0;
    std::vector<double> blocks;
    Impl() { result.levels.emplace_back(); }
    void add(float l, float r) {
        if (result.frames >= MaxFrames) throw std::runtime_error("Analysis exceeds the eight-hour limit.");
        if (!std::isfinite(l) || !std::isfinite(r) || std::abs(l) > 64 || std::abs(r) > 64)
            throw std::runtime_error("Invalid decoded sample.");
        if (result.frames % FramesPerPeak == 0) peak = {l, l, r, r};
        else peak = combine(peak, {l, l, r, r});
        const auto kl = left.process(l), kr = right.process(r);
        energy += kl * kl + kr * kr;
        ++result.frames;
        if (result.frames % FramesPerPeak == 0) result.levels[0].push_back(peak);
        // 400 ms blocks at 100 ms intervals, with complete blocks only.
        if (result.frames % 4800 == 0) {
            energies[hops % 4] = energy / 4800; energy = 0; ++hops;
            if (hops >= 4) blocks.push_back(std::accumulate(energies.begin(), energies.end(), 0.0) / 4);
        }
    }
    Result finish() {
        if (!result.frames) throw std::runtime_error("No audio frames decoded.");
        if (result.frames % FramesPerPeak) result.levels[0].push_back(peak);
        while (result.levels.back().size() > 1) {
            const auto& previous = result.levels.back(); std::vector<Peak> next;
            next.reserve((previous.size() + 1) / 2);
            for (size_t i = 0; i < previous.size(); i += 2)
                next.push_back(i + 1 < previous.size() ? combine(previous[i], previous[i + 1]) : previous[i]);
            result.levels.push_back(std::move(next));
        }
        // Absolute -70 LUFS gate, then -10 LU relative to its gated mean.
        const double absolute = std::pow(10.0, (-70 + .691) / 10);
        double sum = 0; size_t count = 0;
        for (auto block : blocks) if (block > absolute) { sum += block; ++count; }
        if (count) {
            const double threshold = std::max(absolute, sum / static_cast<double>(count) / 10);
            sum = 0; count = 0;
            for (auto block : blocks) if (block > threshold) { sum += block; ++count; }
            if (count) result.loudness = -.691 + 10 * std::log10(sum / static_cast<double>(count));
        }
        return std::move(result);
    }
};

Accumulator::Accumulator() : state(std::make_unique<Impl>()) {}
Accumulator::~Accumulator() = default;
void Accumulator::add(float left, float right) { state->add(left, right); }
uint64_t Accumulator::frames() const { return state->result.frames; }
Result Accumulator::finish() { return state->finish(); }
Result measureSamples(const std::vector<float>& samples) {
    if (samples.size() % 2) throw std::runtime_error("Incomplete stereo frame.");
    Accumulator accumulator;
    for (size_t i = 0; i < samples.size(); i += 2) accumulator.add(samples[i], samples[i+1]);
    return accumulator.finish();
}
}

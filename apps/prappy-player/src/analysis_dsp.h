#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
namespace analysis {
constexpr uint64_t SampleRate = 48000, FramesPerPeak = 1024;
constexpr uint64_t MaxFrames = SampleRate * 60 * 60 * 8;
struct Peak { float leftMin = 0, leftMax = 0, rightMin = 0, rightMax = 0; };
struct Result {
    uint64_t frames = 0;
    std::optional<double> loudness;
    std::vector<std::vector<Peak>> levels;
    double duration() const { return static_cast<double>(frames) / SampleRate; }
};
// Streaming accumulator shared by the Windows pipe decoder and portable DSP tests.
class Accumulator {
    struct Impl;
    std::unique_ptr<Impl> state;
public:
    Accumulator();
    ~Accumulator();
    Accumulator(const Accumulator&) = delete;
    Accumulator& operator=(const Accumulator&) = delete;
    void add(float left, float right);
    uint64_t frames() const;
    Result finish();
};
Result measureSamples(const std::vector<float>& samples);
}

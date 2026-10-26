#pragma once
#include "paths.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>
#include <mutex>
#include <thread>

struct Library;
struct Track;
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
struct Identity {
    fs::path path;
    uint64_t bytes = 0;
    int64_t stamp = 0;
    bool operator==(const Identity&) const = default;
};
Identity identify(const fs::path& path);
using Progress = std::function<void(uint64_t)>;
Result measure(const fs::path& path, std::stop_token stop = {}, const Progress& progress = {});
// Exposed for deterministic signal/reference tests; input is interleaved stereo.
Result measureSamples(const std::vector<float>& samples);
std::string serialize(const Identity& identity, const Result& result);
Result parse(const std::string& bytes, const Identity& expected);
fs::path cachePath(const fs::path& directory, const Identity& identity);

struct Summary {
    Identity identity;
    double duration = 0;
    std::optional<double> loudness;
    std::string error;
    bool cached = false;
};
// One worker, one decode at a time. No filesystem or decoder calls on the UI.
class Worker {
    fs::path directory;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Identity> pending;
    std::vector<Summary> completed;
    std::optional<Identity> active;
    std::stop_source activeStop;
    bool forcePending = false;
    std::jthread thread;
    void run(std::stop_token stop);
public:
    explicit Worker(fs::path cacheDirectory);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void reconcile(const Library& library, bool force = false);
    void prioritize(const Track& track);
    std::vector<Summary> collect();
};
}

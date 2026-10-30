#pragma once
#include "paths.h"
#include "analysis_dsp.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>

struct Library;
struct Track;
namespace analysis {
struct Identity {
    fs::path path;
    uint64_t bytes = 0;
    int64_t stamp = 0;
    bool operator==(const Identity&) const = default;
};
Identity identify(const fs::path& path);
using Progress = std::function<void(uint64_t)>;
using Metadata = std::function<void(const std::vector<std::pair<std::string, std::string>>&, const std::string&)>;
Result measure(const fs::path& path, std::stop_token stop = {}, const Progress& progress = {},
               const Metadata& metadata = {});
std::string serialize(const Identity& identity, const Result& result);
Result parse(const std::string& bytes, const Identity& expected);

struct Summary {
    Identity identity;
    double duration = 0;
    std::optional<double> loudness;
    std::string error;
    bool cached = false;
    float samplePeak = 0;
    std::shared_ptr<const Result> waveform;
};
}

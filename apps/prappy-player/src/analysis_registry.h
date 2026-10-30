#pragma once
// Portable, bounded, checksummed analysis summary format. Waveform data is
// never deserialized here; it has an independent artifact and validity.
#include "gain_policy.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace analysis_registry {
namespace fs = std::filesystem;
constexpr uint64_t Algorithm = 2; // decoded stereo float/48 kHz; sample peaks, not true peaks
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
enum class State : uint64_t { Missing, Queued, Running, Ready, Failed, Offline, Cancelled };
struct Entry {
    std::string path;                // UTF-8 canonical path (not source file bytes)
    uint64_t bytes = 0;
    int64_t stamp = 0;
    uint64_t algorithm = Algorithm;
    State state = State::Missing;
    uint64_t frames = 0;
    double duration = 0;
    std::optional<double> loudness;
    std::optional<double> samplePeak; // decoded stereo 48k float PCM, mandatory codec gain included
    std::string waveform;           // relative artifact name; empty if absent
    gain::Tags tags;                // known tag status; unknown is represented by absent/no probe
    bool tagsProbed = false;
    uint64_t retryAfter = 0;       // wall clock seconds, failures only
    std::string error;
    uint64_t elapsedMs = 0;
};
using Entries = std::map<std::string, Entry>;
uint64_t checksum(std::string_view data);
std::string encode(const Entries& entries);
Entries decode(std::string_view data);
std::string pathUtf8(const fs::path& path);
fs::path pathFromUtf8(const std::string& text);
std::string blobName(const Entry& value);
void atomicWrite(const fs::path& path, const std::string& bytes);
std::string readFile(const fs::path& path);
}

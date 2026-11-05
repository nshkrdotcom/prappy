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

namespace analysis {
using Progress = std::function<void(uint64_t)>;
using Metadata = std::function<void(const std::vector<std::pair<std::string, std::string>>&, const std::string&)>;
Result measure(const fs::path& path, std::stop_token stop = {}, const Progress& progress = {},
               const Metadata& metadata = {});
}

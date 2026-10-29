#pragma once
#include <filesystem>
int selfTest(const std::filesystem::path& config, const std::filesystem::path& logFile);
int stateTests(const std::filesystem::path& config, const std::filesystem::path& logFile);
int outputMeterTests(const std::filesystem::path& signal, const std::filesystem::path& logFile);

#pragma once
#include "paths.h"
#include "state_io.h"
#include <shlobj.h>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <memory>
struct Config {
    std::vector<fs::path> roots;
    std::set<std::string> extensions;
    size_t historyLimit = 10000;
    bool addNewListeningHistoryEntryWhenReplayingHistoryItem = false;
    double normalizationTargetLufs = -18, normalizationUnmeasuredFallbackDb = 0;
    unsigned analysisWorkerCount = 0, analysisRemoteConcurrencyLimit = 2;
    unsigned analysisWaveformMemoryMiB = 128;
};
inline Config readConfig(const fs::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot read player.ini beside the executable.");
    Config config;
    std::string line; int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Config line " + std::to_string(lineNumber) + ": expected key=value.");
        const auto name = lower(trim(line.substr(0, equal)));
        auto value = trim(line.substr(equal + 1));
        if (name == "directory") {
            if (value.empty()) throw std::runtime_error("Config: directory cannot be empty.");
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            fs::path path = std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size());
            if (path.is_relative()) path = file.parent_path() / path;
            config.roots.push_back(fs::absolute(path).lexically_normal());
        } else if (name == "extensions") {
            std::istringstream values(value); std::string ext;
            while (std::getline(values, ext, ',')) {
                ext = lower(trim(ext)); if (ext.starts_with('.')) ext.erase(0, 1);
                if (ext.empty() || ext.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos)
                    throw std::runtime_error("Config: invalid extension.");
                config.extensions.insert('.' + ext);
            }
        } else if (name == "add_new_listening_history_entry_when_replaying_history_item") {
            value=lower(value);
            if(value!="true"&&value!="false")throw std::runtime_error("Config: add_new_listening_history_entry_when_replaying_history_item must be true or false.");
            config.addNewListeningHistoryEntryWhenReplayingHistoryItem=value=="true";
        } else if (name == "normalization_target_lufs" || name == "normalization_unmeasured_fallback_db") {
            double number = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number, std::chars_format::general);
            const bool target = name == "normalization_target_lufs";
            if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number) ||
                (target && (number < -36 || number > -5)) || (!target && (number < -24 || number > 12)))
                throw std::runtime_error("Config: invalid " + name + " (target -36..-5 LUFS; fallback -24..12 dB).");
            if (target) config.normalizationTargetLufs = number;
            else config.normalizationUnmeasuredFallbackDb = number;
        } else if (name == "analysis_worker_count" || name == "analysis_remote_concurrency_limit" || name == "analysis_waveform_memory_mib") {
            unsigned number = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() ||
                (name == "analysis_worker_count" && number > 4) ||
                (name == "analysis_remote_concurrency_limit" && (number < 1 || number > 4)) ||
                (name == "analysis_waveform_memory_mib" && (number < 16 || number > 512)))
                throw std::runtime_error("Config: invalid " + name + " (workers 0..4, remote 1..4, memory 16..512 MiB).");
            if (name == "analysis_worker_count") config.analysisWorkerCount = number;
            else if (name == "analysis_remote_concurrency_limit") config.analysisRemoteConcurrencyLimit = number;
            else config.analysisWaveformMemoryMiB = number;
        } else if (name == "history_limit") {
            size_t limit = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (error != std::errc{} || end != value.data() + value.size() || limit == 0 || limit > stateio::MaxRecords)
                throw std::runtime_error("Config: history_limit must be between 1 and 1000000.");
            config.historyLimit = limit;
        } else throw std::runtime_error("Config line " + std::to_string(lineNumber) + ": unknown key " + name);
    }
    if (config.roots.empty() || config.extensions.empty()) throw std::runtime_error("Config needs directory and extensions entries.");
    return config;
}
inline void createDefaultConfig(const fs::path& file) {
    if (fs::exists(file)) return;
    PWSTR music = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music))) throw std::runtime_error("Cannot locate your Music folder.");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(music, CoTaskMemFree);
    const auto root = utf8(fs::path(music));
    std::ofstream out(file);
    out << "# UTF-8. Repeat directory= for more roots. Relative roots resolve beside this file.\n"
        << "directory=" << root << "\nextensions=flac,wav,mp3,opus,m4a,wv,mka,webm,ogg,aac,aiff,wma\n"
        << "history_limit=10000\nadd_new_listening_history_entry_when_replaying_history_item=false\n"
        << "# Loudness matching is enabled/disabled in Options and saved with player-ui.txt.\n"
        << "normalization_target_lufs=-18\nnormalization_unmeasured_fallback_db=0\n"
        << "# 0 = auto: hardware cores minus two, clamped to 1..4.\n"
        << "analysis_worker_count=0\nanalysis_remote_concurrency_limit=2\nanalysis_waveform_memory_mib=128\n";
    if (!out) throw std::runtime_error("Cannot create player.ini. Put the player in a writable folder.");
}

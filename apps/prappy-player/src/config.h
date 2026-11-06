#pragma once
#include "paths.h"
#include "state_io.h"
#include <charconv>
#include <cmath>
#include <fstream>
struct Config {
    size_t historyLimit = 10000, editorPcmMib = 256;
    bool addNewListeningHistoryEntryWhenReplayingHistoryItem = false;
    bool debugLog = true;
    double normalizationTargetLufs = -18, normalizationUnmeasuredFallbackDb = 0;
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
        if (name == "debug_log") {
            value=lower(value);
            if(value!="true"&&value!="false")throw std::runtime_error("Config: debug_log must be true or false.");
            config.debugLog=value=="true";
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
        } else if (name == "history_limit" || name == "editor_pcm_mib") {
            size_t limit = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
            const bool editor=name=="editor_pcm_mib";
            if (error != std::errc{} || end != value.data() + value.size() || limit < (editor?64:1) || limit > (editor?1024:stateio::MaxRecords))
                throw std::runtime_error(editor?"Config: editor_pcm_mib must be 64..1024.":"Config: history_limit must be 1..1000000.");
            if(editor)config.editorPcmMib=limit;else config.historyLimit=limit;
        } else throw std::runtime_error("Config line " + std::to_string(lineNumber) + ": unknown key " + name);
    }
    return config;
}
inline void createDefaultConfig(const fs::path& file) {
    if (fs::exists(file)) return;
    std::ofstream out(file);
    out << "# Libraries are configured by audioharvest's workstation.json in WSL.\n"
        << "history_limit=10000\nadd_new_listening_history_entry_when_replaying_history_item=false\ndebug_log=true\neditor_pcm_mib=256\n"
        << "# Loudness matching is enabled/disabled in Options and saved with player-ui.txt.\n"
        << "normalization_target_lufs=-18\nnormalization_unmeasured_fallback_db=0\n";
    if (!out) throw std::runtime_error("Cannot create player.ini. Put the player in a writable folder.");
}

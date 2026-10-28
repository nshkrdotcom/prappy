#pragma once
#include "paths.h"
#include "state_io.h"
#include <shlobj.h>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <memory>
struct Config { std::vector<fs::path> roots; std::set<std::string> extensions; size_t historyLimit = 10000; };
inline Config readConfig(const fs::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot read player.ini beside the executable.");
    Config config;
    std::string line; int number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Config line " + std::to_string(number) + ": expected key=value.");
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
        } else if (name == "history_limit") {
            size_t limit = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (error != std::errc{} || end != value.data() + value.size() || limit == 0 || limit > stateio::MaxRecords)
                throw std::runtime_error("Config: history_limit must be between 1 and 1000000.");
            config.historyLimit = limit;
        } else throw std::runtime_error("Config line " + std::to_string(number) + ": unknown key " + name);
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
        << "directory=" << root << "\nextensions=flac,wav,mp3,opus,m4a,wv,mka,webm,ogg,aac,aiff,wma\nhistory_limit=10000\n";
    if (!out) throw std::runtime_error("Cannot create player.ini. Put the player in a writable folder.");
}

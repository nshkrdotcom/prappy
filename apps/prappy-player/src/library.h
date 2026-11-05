#pragma once
#include "config.h"
#include "state_io.h"
#include "prepared_catalog.h"
#include <atomic>
#include <optional>
#include <set>
#include <stop_token>
#include <tuple>
#include <map>
#include <memory>
#include <vector>
namespace sampling {struct Connection;}
struct Track {
    fs::path path;
    std::string name, directory, type;
    std::wstring id;
    uintmax_t bytes = 0;
    std::vector<fs::path> copies;
    std::string label;
    bool offline = false;
    fs::path sourceRoot;
    std::wstring search;
    std::wstring searchName;
    size_t searchLabelLength = 0;
    std::string sortName;
    std::optional<double> duration;
    std::optional<prepared::CatalogSource> prepared;
};
struct ScanProgress {
    std::atomic<size_t> found{0};
};
struct Library {
    std::shared_ptr<const sampling::Connection> backend;
    std::string backendDistribution;
    std::map<std::wstring,std::string> labels;
    std::vector<Track> tracks;
    std::vector<fs::path> roots;
    std::set<std::string> extensions;
    size_t historyLimit = 10000;
    bool addNewListeningHistoryEntryWhenReplayingHistoryItem = false;
    int order = 0;
    std::string notice;
    std::string pathLabel(const fs::path& path) const {
        const auto found=labels.find(key(path));return found==labels.end()?readablePath(path,roots):found->second;
    }
    static bool matches(const Track& track, const std::wstring& id) {
        return track.id == id || std::any_of(track.copies.begin(), track.copies.end(), [&](auto& p) { return key(p) == id; });
    }
    static fs::path pathInFolder(const Track& track, const fs::path& folder) {
        const auto id = key(folder);
        if (key(track.path.parent_path()) == id) return track.path;
        const auto copy = std::find_if(track.copies.begin(), track.copies.end(), [&](const auto& path) { return key(path.parent_path()) == id; });
        return copy == track.copies.end() ? fs::path{} : *copy;
    }
    void rebuildSearch() {
        for (auto& track : tracks) {
            track.sortName = lower(track.name);
            track.search = searchText(track.label);
            track.searchLabelLength = track.search.size(); track.searchName = searchText(track.name);
            for (const auto& copy : track.copies) track.search += L'\n' + searchText(pathLabel(copy));
        }
    }
    void sort() {
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            if (order == 3 && a.duration != b.duration) {
                if (!a.duration || !b.duration) return a.duration.has_value();
                return *a.duration < *b.duration;
            }
            if (order == 1 && a.directory != b.directory) return a.directory < b.directory;
            if (order == 2 && a.type != b.type) return a.type < b.type;
            return a.sortName == b.sortName ? a.id < b.id : a.sortName < b.sortName;
        });
    }
};

#pragma once
#include "library.h"
#include "history.h"
#include "analysis.h"
#include "directory_filter.h"
#include <cmath>
#include <cwctype>

struct ListeningStats { size_t count = 0; int64_t last = 0; };
struct Browse {
    enum class View { All, Unplayed, MostPlayed, LastPlayed };
    View view = View::All;
    int availability = 0;
    std::string extension;
    double minimum = 0, maximum = 0;
    bool measuredOnly = false;
    DirectoryFilter directories;
    std::map<std::wstring, ListeningStats> stats;
    std::map<std::wstring, std::wstring> canonical;
    void rebuild(const Library& library, const History& history, bool reindex = true) {
        if (reindex) {
            canonical.clear();
            for (const auto& track : library.tracks) {
                canonical.emplace(track.id, track.id);
                for (const auto& copy : track.copies) canonical.emplace(key(copy), track.id);
            }
        }
        stats.clear();
        for (const auto& entry : history.entries) if (!entry.queued) {
            const auto alias = canonical.find(key(entry.path));
            if (alias != canonical.end()) { auto& value = stats[alias->second]; ++value.count; value.last = std::max(value.last, entry.time); }
        }
    }
    ListeningStats played(const Track& track) const {
        const auto found = stats.find(track.id); return found == stats.end() ? ListeningStats{} : found->second;
    }
    static std::vector<std::wstring> tokens(const std::string& query) {
        std::vector<std::wstring> result;
        std::wstring term; bool quoted = false;
        const auto flush = [&] { if (!term.empty()) { result.push_back(std::move(term)); term.clear(); } };
        for (const auto character : searchText(query)) {
            if (character == L'"') quoted = !quoted;
            else if (!quoted && std::iswspace(character)) flush();
            else term += character;
        }
        flush(); return result;
    }
    static int score(const Track& track, const std::vector<std::wstring>& terms) {
        int rank = 0; const auto& name = track.searchName;
        for (const auto& term : terms) {
            if (track.search.find(term) == std::wstring::npos) return -1;
            if (name == term) rank += 100;
            else if (name.starts_with(term)) rank += 50;
            else if (name.find(term) != std::wstring::npos) rank += 25;
            else if (track.search.find(term) + term.size() <= track.searchLabelLength) rank += 10;
            else ++rank;
        }
        return rank;
    }
    std::vector<size_t> select(const Library& library, const std::string& query) const {
        const auto terms = tokens(query); std::vector<std::pair<size_t, int>> matches;
        const bool scoped = directories.active(), sortFolders = scoped && terms.empty() && view == View::All && library.order == 1;
        std::vector<std::wstring> folders; if (sortFolders) folders.resize(library.tracks.size());
        for (size_t i = 0; i < library.tracks.size(); ++i) {
            const auto& track = library.tracks[i]; const auto activity = view == View::All ? ListeningStats{} : played(track);
            fs::path allowed;
            if (scoped) { allowed = directories.playbackPath(track); if (allowed.empty()) continue; }
            if ((view == View::Unplayed && activity.count) ||
                ((view == View::MostPlayed || view == View::LastPlayed) && !activity.count) ||
                (availability == 1 && track.offline) || (availability == 2 && !track.offline) ||
                (!extension.empty() && track.type != extension) ||
                (!track.duration && (measuredOnly || minimum > 0 || maximum > 0)) ||
                (track.duration && (*track.duration < minimum || (maximum > 0 && *track.duration > maximum)))) continue;
            int rank = 0;
            if (scoped && !terms.empty()) {
                Track searchable; searchable.searchName = searchText(utf8(allowed.filename())); searchable.search = searchText(library.pathLabel(allowed));
                searchable.searchLabelLength = searchable.search.size();
                for (const auto& copy : track.copies) if (directories.accepts(copy)) searchable.search += L'\n' + searchText(library.pathLabel(copy));
                rank = score(searchable, terms);
            } else rank = score(track, terms);
            if (rank >= 0) { matches.emplace_back(i, rank); if (sortFolders) folders[i] = directoryKey(allowed.parent_path()); }
        }
        if (!terms.empty() || view == View::MostPlayed || view == View::LastPlayed) std::stable_sort(matches.begin(), matches.end(), [&](const auto& a, const auto& b) {
            if (!terms.empty() && a.second != b.second) return a.second > b.second;
            const auto left = played(library.tracks[a.first]), right = played(library.tracks[b.first]);
            if (view == View::MostPlayed && left.count != right.count) return left.count > right.count;
            if (view == View::LastPlayed && left.last != right.last) return left.last > right.last;
            return false;
        });
        if (sortFolders)
            std::stable_sort(matches.begin(), matches.end(), [&](const auto& a, const auto& b) {
                return folders[a.first] < folders[b.first];
            });
        std::vector<size_t> result; result.reserve(matches.size());
        for (const auto& match : matches) result.push_back(match.first); return result;
    }
};

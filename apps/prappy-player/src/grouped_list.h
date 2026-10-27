#pragma once
#include "library.h"
#include <set>

// Headers and tracks share one row height and one viewport clipper. Rebuild only
// when the filtered catalog or a group's open state changes, never while scrolling.
struct GroupedList {
    struct Group { std::string key, label; size_t begin, end; bool open; };
    struct Row { bool header; size_t index; };
    std::vector<Group> groups;
    std::vector<Row> rows;
    std::set<std::pair<int, std::string>> closed;
    int order = 0;
    bool dirty = true;
    template<class GroupName> void rebuild(const Library& library, const std::vector<size_t>& visible, GroupName name) {
        order = library.order; groups.clear();
        for (size_t begin = 0; begin < visible.size();) {
            const auto key = name(library.tracks[visible[begin]]);
            size_t end = begin + 1;
            while (end < visible.size() && name(library.tracks[visible[end]]) == key) ++end;
            groups.push_back({key, key + " (" + std::to_string(end - begin) + ")", begin, end, !closed.contains({order, key})});
            begin = end;
        }
        dirty = true;
    }
    void rebuild(const Library& library, const std::vector<size_t>& visible) {
        rebuild(library, visible, [&](const Track& track) { return library.order == 1 ? track.directory : track.type; });
    }
    void flatten(const std::vector<size_t>& visible) {
        if (!dirty) return;
        rows.clear();
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto& group = groups[i]; rows.push_back({true, i});
            if (group.open) for (size_t j = group.begin; j < group.end; ++j) rows.push_back({false, visible[j]});
        }
        dirty = false;
    }
    void toggle(size_t index, bool open) {
        auto& group = groups[index]; group.open = open;
        if (open) closed.erase({order, group.key}); else closed.emplace(order, group.key);
        dirty = true;
    }
};

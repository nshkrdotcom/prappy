#pragma once
#include "library.h"

inline std::wstring directoryKey(const fs::path& path) {
    auto value = key(displayPath(path));
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}
inline bool inDirectory(const std::wstring& path, const std::wstring& directory) {
    if (directory.empty()) return false;
    return path == directory || (path.starts_with(directory) &&
        (directory.back() == L'\\' || (path.size() > directory.size() && path[directory.size()] == L'\\')));
}
struct DirectoryFilter {
    struct Rule { fs::path path; std::wstring id; bool operator==(const Rule&) const = default; };
    std::vector<Rule> included, excluded;
    static bool contains(const std::vector<Rule>& rules, const fs::path& path) {
        const auto id = directoryKey(path);
        return std::any_of(rules.begin(), rules.end(), [&](const auto& rule) { return rule.id == id; });
    }
    static void set(std::vector<Rule>& rules, const fs::path& path, bool enabled) {
        const auto id = directoryKey(path);
        std::erase_if(rules, [&](const auto& rule) { return rule.id == id; });
        if (enabled) rules.push_back({displayPath(path).lexically_normal(), id});
    }
    bool active() const { return !included.empty() || !excluded.empty(); }
    bool accepts(const fs::path& path) const {
        if (!active()) return true;
        const auto id = directoryKey(path);
        const auto matches = [&](const auto& rule) { return inDirectory(id, rule.id); };
        return !std::any_of(excluded.begin(), excluded.end(), matches) &&
            (included.empty() || std::any_of(included.begin(), included.end(), matches));
    }
    fs::path playbackPath(const Track& track) const {
        if (!active() || accepts(track.path)) return track.path;
        for (const auto& copy : track.copies) if (accepts(copy)) return copy;
        return {};
    }
    void only(const fs::path& path) { included.clear(); set(included, path, true); }
    std::vector<fs::path> exclusions() const {
        std::vector<fs::path> result; for (const auto& rule : excluded) result.push_back(rule.path); return result;
    }
};

struct DirectoryIndex {
    struct Choice { fs::path path; std::wstring id; std::string label; size_t count = 0; bool hasChildren = false; };
    struct Node { fs::path path; std::wstring id; std::string label; size_t count = 0; std::vector<std::wstring> children; };
    std::map<std::wstring, Node> nodes;
    std::vector<std::wstring> roots, featured;
    std::vector<Choice> priorityChoices(const DirectoryFilter& filter) const {
        std::vector<Choice> result; std::set<std::wstring> seen;
        const auto add = [&](const fs::path& path) {
            const auto id = directoryKey(path); if (!seen.insert(id).second) return;
            const auto found = nodes.find(id);
            if (found == nodes.end()) result.push_back({path,id,utf8(path.filename().empty() ? path : path.filename()),0,false});
            else { const auto& node = found->second; result.push_back({node.path,id,node.label,node.count,!node.children.empty()}); }
        };
        for (const auto& rule : filter.included) add(rule.path);
        for (const auto& rule : filter.excluded) add(rule.path);
        for (const auto& id : featured) add(nodes.at(id).path);
        return result;
    }
    uint64_t revision = 0;
    void rebuild(const Library& library) {
        nodes.clear(); roots.clear();
        ++revision;
        std::set<std::pair<std::wstring, std::wstring>> edges;
        const auto add = [&](const fs::path& path) -> Node& {
            const auto id = directoryKey(path); auto [it, fresh] = nodes.try_emplace(id);
            if (fresh) it->second = {path, id, utf8(path.filename().empty() ? path : path.filename()), 0, {}};
            return it->second;
        };
        for (const auto& track : library.tracks) {
            std::set<std::wstring> counted;
            auto visit = [&](const fs::path& file) {
                const auto id = directoryKey(file); fs::path root;
                for (const auto& candidate : library.roots) if (inDirectory(id, directoryKey(candidate)) &&
                    (root.empty() || directoryKey(candidate).size() < directoryKey(root).size())) root = candidate;
                if (root.empty()) root = file.parent_path();
                const auto rootId = directoryKey(root);
                if (std::find(roots.begin(), roots.end(), rootId) == roots.end()) roots.push_back(rootId);
                auto path = file.parent_path();
                for (;;) {
                    auto& node = add(path); if (counted.insert(node.id).second) ++node.count;
                    if (node.id == rootId || path == path.parent_path()) break;
                    auto& parent = add(path.parent_path());
                    if (edges.emplace(parent.id, node.id).second) parent.children.push_back(node.id);
                    path = path.parent_path();
                }
            };
            visit(track.path); for (const auto& copy : track.copies) visit(copy);
        }
        const auto less = [&](const auto& a, const auto& b) { return nodes.at(a).count != nodes.at(b).count ? nodes.at(a).count > nodes.at(b).count : a < b; };
        std::sort(roots.begin(), roots.end(), less);
        for (auto& [id, node] : nodes) { (void)id; std::sort(node.children.begin(), node.children.end(), less); }
    }
    void prioritize(const std::vector<fs::path>& pins) {
        ++revision;
        featured.clear();
        std::set<std::wstring> seen;
        for (const auto& pin : pins) { const auto id = directoryKey(pin); if (nodes.contains(id) && seen.insert(id).second) featured.push_back(id); }
        std::vector<std::wstring> candidates;
        for (const auto& root : roots) {
            const auto& children = nodes.at(root).children;
            if (children.empty()) candidates.push_back(root); else candidates.insert(candidates.end(), children.begin(), children.end());
        }
        std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
            return nodes.at(a).count != nodes.at(b).count ? nodes.at(a).count > nodes.at(b).count : a < b;
        });
        for (const auto& id : candidates) if (seen.insert(id).second) featured.push_back(id);
    }
};

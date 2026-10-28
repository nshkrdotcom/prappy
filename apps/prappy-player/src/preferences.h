#pragma once
#include "state_io.h"
#include "paths.h"
#include "transport_layout.h"
#include <iomanip>
#include <sstream>
#include <set>
#include <cmath>

struct Preferences {
    struct Session {
        bool expanded = false, history = false, positioned = false, maximized = false, normalMaximized = false;
        int x = 0, y = 0, width = 500, height = 250, expandedHeight = 710, normalHeight = 250;
        int view = 0, order = 0, availability = 0;
        double minimum = 0, maximum = 0;
        bool measuredOnly = false;
        std::string search, extension, directorySearch;
        fs::path directoryLocation;
        std::vector<fs::path> includedDirectories;
        std::set<std::pair<int, std::string>> closedGroups;
        float scrollX = 0, scrollY = 0;
        bool operator==(const Session&) const = default;
    } session;
    bool topmost = false, strip = false, normalize = false;
    fs::path folder;
    int transportHeight = TransportLayout::Compact;
    int volume = 70;
    std::vector<fs::path> hiddenDirectories, pinnedDirectories;
    std::string serialize() const {
        std::ostringstream out;
        out << "PRAPPY_UI 4\n" << topmost << ' ' << strip << ' ' << normalize << ' ' << transportHeight << '\n' << std::quoted(stateio::pathText(folder)) << '\n';
        for (const auto* paths : {&hiddenDirectories, &pinnedDirectories}) {
            out << paths->size() << '\n'; for (const auto& path : *paths) out << std::quoted(stateio::pathText(path)) << '\n';
        }
        out << volume << '\n';
        const auto& s = session;
        out << s.expanded << ' ' << s.history << ' ' << s.positioned << ' ' << s.maximized << ' ' << s.normalMaximized << '\n'
            << s.x << ' ' << s.y << ' ' << s.width << ' ' << s.height << ' ' << s.expandedHeight << ' ' << s.normalHeight << '\n'
            << s.view << ' ' << s.order << ' ' << s.availability << ' ' << std::setprecision(17) << s.minimum << ' ' << s.maximum << ' ' << s.measuredOnly << '\n'
            << std::quoted(s.search) << ' ' << std::quoted(s.extension) << ' ' << std::quoted(s.directorySearch) << ' ' << std::quoted(stateio::pathText(s.directoryLocation)) << '\n'
            << s.scrollX << ' ' << s.scrollY << '\n' << s.includedDirectories.size() << '\n';
        for (const auto& path : s.includedDirectories) out << std::quoted(stateio::pathText(path)) << '\n';
        out << s.closedGroups.size() << '\n';
        for (const auto& [order, group] : s.closedGroups) out << order << ' ' << std::quoted(group) << '\n';
        return out.str();
    }
    static Preferences load(const fs::path& file) {
        Preferences value;
        try {
            std::istringstream in(stateio::read(file)); std::string header, folder;
            int top = 0, slim = 0, gain = 0;
            if (!std::getline(in, header) || (header != "PRAPPY_UI 2" && header != "PRAPPY_UI 3" && header != "PRAPPY_UI 4") || !(in >> top >> slim >> gain >> value.transportHeight >> std::quoted(folder)) ||
                top < 0 || top > 1 || slim < 0 || slim > 1 || gain < 0 || gain > 1 || value.transportHeight < TransportLayout::Compact || value.transportHeight > TransportLayout::Maximum) return {};
            for (auto* paths : {&value.hiddenDirectories, &value.pinnedDirectories}) {
                size_t count = 0; if (!(in >> count) || count > 4096) return {};
                for (size_t i = 0; i < count; ++i) { std::string path; if (!(in >> std::quoted(path))) return {}; paths->push_back(stateio::pathFromText(path)); }
            }
            if (header != "PRAPPY_UI 2" && (!(in >> value.volume) || value.volume < 0 || value.volume > 100)) return {};
            if (header == "PRAPPY_UI 4") {
                auto& s = value.session;
                std::string location;
                if (!(in >> s.expanded >> s.history >> s.positioned >> s.maximized >> s.normalMaximized
                    >> s.x >> s.y >> s.width >> s.height >> s.expandedHeight >> s.normalHeight
                    >> s.view >> s.order >> s.availability >> s.minimum >> s.maximum >> s.measuredOnly
                    >> std::quoted(s.search) >> std::quoted(s.extension) >> std::quoted(s.directorySearch) >> std::quoted(location)
                    >> s.scrollX >> s.scrollY)) return {};
                if (s.x < -1000000 || s.x > 1000000 || s.y < -1000000 || s.y > 1000000 ||
                    s.width < 500 || s.width > 32768 || s.height < 86 || s.height > 32768 ||
                    s.normalHeight < 250 || s.normalHeight > 32768 || s.expandedHeight < 250 || s.expandedHeight > 32768 ||
                    s.view < 0 || s.view > 3 || s.order < 0 || s.order > 3 || s.availability < 0 || s.availability > 2 ||
                    !std::isfinite(s.minimum) || !std::isfinite(s.maximum) || s.minimum < 0 || s.maximum < 0 || s.minimum > 28800 || s.maximum > 28800 ||
                    !std::isfinite(s.scrollX) || !std::isfinite(s.scrollY) || s.scrollX < 0 || s.scrollY < 0 ||
                    s.search.size() >= 256 || s.directorySearch.size() >= 128 || s.extension.size() > 128) return {};
                if (!location.empty()) s.directoryLocation = stateio::pathFromText(location);
                size_t count = 0;
                if (!(in >> count) || count > 4096) return {};
                for (size_t i = 0; i < count; ++i) { std::string path; if (!(in >> std::quoted(path))) return {}; s.includedDirectories.push_back(stateio::pathFromText(path)); }
                if (!(in >> count) || count > stateio::MaxRecords) return {};
                for (size_t i = 0; i < count; ++i) {
                    int order = 0; std::string group;
                    if (!(in >> order >> std::quoted(group)) || order < 0 || order > 3 || group.size() > stateio::MaxString) return {};
                    s.closedGroups.emplace(order, std::move(group));
                }
            }
            in >> std::ws; if (!in.eof()) return {};
            value.topmost = top != 0; value.strip = slim != 0; value.normalize = gain != 0;
            if (!folder.empty()) value.folder = stateio::pathFromText(folder);
        } catch (const std::exception&) { return {}; }
        return value;
    }
};

#pragma once
#include "state_io.h"
#include "paths.h"
#include "transport_layout.h"
#include <iomanip>
#include <sstream>

struct Preferences {
    bool topmost = false, strip = false, normalize = false;
    fs::path folder;
    int transportHeight = TransportLayout::Compact;
    std::vector<fs::path> hiddenDirectories, pinnedDirectories;
    std::string serialize() const {
        std::ostringstream out;
        out << "PRAPPY_UI 2\n" << topmost << ' ' << strip << ' ' << normalize << ' ' << transportHeight << '\n' << std::quoted(stateio::pathText(folder)) << '\n';
        for (const auto* paths : {&hiddenDirectories, &pinnedDirectories}) {
            out << paths->size() << '\n'; for (const auto& path : *paths) out << std::quoted(stateio::pathText(path)) << '\n';
        }
        return out.str();
    }
    static Preferences load(const fs::path& file) {
        Preferences value;
        try {
            std::istringstream in(stateio::read(file)); std::string header, folder;
            int top = 0, slim = 0, gain = 0;
            if (!std::getline(in, header) || header != "PRAPPY_UI 2" || !(in >> top >> slim >> gain >> value.transportHeight >> std::quoted(folder)) ||
                top < 0 || top > 1 || slim < 0 || slim > 1 || gain < 0 || gain > 1 || value.transportHeight < TransportLayout::Compact || value.transportHeight > TransportLayout::Maximum) return {};
            for (auto* paths : {&value.hiddenDirectories, &value.pinnedDirectories}) {
                size_t count = 0; if (!(in >> count) || count > 4096) return {};
                for (size_t i = 0; i < count; ++i) { std::string path; if (!(in >> std::quoted(path))) return {}; paths->push_back(stateio::pathFromText(path)); }
            }
            in >> std::ws; if (!in.eof()) return {};
            value.topmost = top != 0; value.strip = slim != 0; value.normalize = gain != 0;
            if (!folder.empty()) value.folder = stateio::pathFromText(folder);
        } catch (const std::exception&) { return {}; }
        return value;
    }
};

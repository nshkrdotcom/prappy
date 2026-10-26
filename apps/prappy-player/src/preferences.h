#pragma once
#include "state_io.h"
#include "paths.h"
#include <iomanip>
#include <sstream>

struct Preferences {
    bool topmost = false, strip = false, normalize = false;
    fs::path folder;
    std::string serialize() const {
        std::ostringstream out;
        out << "PRAPPY_UI 1\n" << topmost << ' ' << strip << ' ' << normalize << '\n' << std::quoted(stateio::pathText(folder)) << '\n';
        return out.str();
    }
    static Preferences load(const fs::path& file) {
        Preferences value;
        try {
            std::istringstream in(stateio::read(file)); std::string header, folder;
            int top = 0, slim = 0, gain = 0;
            if (!std::getline(in, header) || header != "PRAPPY_UI 1" || !(in >> top >> slim >> gain >> std::quoted(folder)) ||
                top < 0 || top > 1 || slim < 0 || slim > 1 || gain < 0 || gain > 1) return {};
            in >> std::ws; if (!in.eof()) return {};
            value.topmost = top != 0; value.strip = slim != 0; value.normalize = gain != 0;
            if (!folder.empty()) value.folder = stateio::pathFromText(folder);
        } catch (const std::exception&) { return {}; }
        return value;
    }
};

#pragma once
#include <string>
inline std::string statusText(std::string primary, const std::string& progress = {}) {
    if (!progress.empty()) primary += (primary.empty() ? "" : " \xc2\xb7 ") + progress;
    for (size_t offset = 0; (offset = primary.find(" | ", offset)) != std::string::npos; offset += 4)
        primary.replace(offset, 3, " \xc2\xb7 ");
    return primary;
}

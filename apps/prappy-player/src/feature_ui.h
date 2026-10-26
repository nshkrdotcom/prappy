#pragma once
#include "analysis.h"
#include "browse.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "text.h"
#include <shellapi.h>
#include <shlobj.h>

inline bool revealFile(HWND window, const fs::path& path) {
    PIDLIST_ABSOLUTE item = nullptr;
    const auto result = SHParseDisplayName(displayPath(path).c_str(), nullptr, &item, 0, nullptr);
    if (FAILED(result)) return false;
    const auto opened = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item); (void)window;
    return SUCCEEDED(opened);
}

inline bool waveformSeek(const analysis::Result* waveform, double& position, double duration, ImVec2 size) {
    ImGui::InvisibleButton("##waveform", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(24, 29, 31, 255), 3);
    const auto ratio = duration > 0 ? std::clamp(position / duration, 0.0, 1.0) : 0;
    const auto playX = min.x + static_cast<float>(ratio) * size.x;
    if (waveform && !waveform->levels.empty()) {
        const auto& maximum = waveform->levels.back().front();
        const auto amplitude = std::max({std::abs(maximum.leftMin), std::abs(maximum.leftMax), std::abs(maximum.rightMin), std::abs(maximum.rightMax)});
        const float displayScale = amplitude > 1e-12f ? .9f / amplitude : 1;
        size_t level = 0;
        while (level + 1 < waveform->levels.size() && waveform->levels[level].size() > static_cast<size_t>(size.x * 2)) ++level;
        const auto& peaks = waveform->levels[level];
        const int columns = std::clamp(static_cast<int>(size.x), 1, 4096);
        const double framesPerBin = static_cast<double>(analysis::FramesPerPeak * (1ULL << level));
        for (int column = 0; column < columns; ++column) {
            const auto begin = static_cast<size_t>(static_cast<double>(column) / columns * static_cast<double>(waveform->frames) / framesPerBin);
            const auto end = std::min(peaks.size(), static_cast<size_t>(std::ceil(static_cast<double>(column + 1) / columns * static_cast<double>(waveform->frames) / framesPerBin)));
            if (begin >= peaks.size()) continue;
            auto peak = peaks[begin];
            for (size_t i = begin + 1; i < end; ++i) {
                peak.leftMin = std::min(peak.leftMin, peaks[i].leftMin); peak.leftMax = std::max(peak.leftMax, peaks[i].leftMax);
                peak.rightMin = std::min(peak.rightMin, peaks[i].rightMin); peak.rightMax = std::max(peak.rightMax, peaks[i].rightMax);
            }
            const float x = min.x + static_cast<float>(column) / static_cast<float>(columns) * size.x;
            const auto color = x <= playX ? IM_COL32(64, 224, 177, 255) : IM_COL32(103, 126, 127, 255);
            const auto lane = [&](float low, float high, float middle) {
                const float top = min.y + size.y * middle - std::clamp(high * displayScale, -1.0f, 1.0f) * size.y * .22f;
                const float bottom = min.y + size.y * middle - std::clamp(low * displayScale, -1.0f, 1.0f) * size.y * .22f;
                draw->AddLine({x, top}, {x, std::max(top + 1, bottom)}, color);
            };
            lane(peak.leftMin, peak.leftMax, .25f); lane(peak.rightMin, peak.rightMax, .75f);
        }
    } else {
        draw->AddLine({min.x, min.y + size.y * .5f}, {max.x, min.y + size.y * .5f}, IM_COL32(83, 99, 103, 255));
    }
    draw->AddLine({playX, min.y}, {playX, max.y}, IM_COL32(245, 189, 99, 255), 2);
    if (ImGui::IsItemFocused()) draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavCursor), 3);
    bool changed = false;
    if (duration > 0 && ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        position = duration * std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - min.x) / size.x), 0.0, 1.0); changed = true;
    }
    if (duration > 0 && ImGui::IsItemFocused()) {
        for (const auto key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_Home, ImGuiKey_End})
            ImGui::SetKeyOwner(key, ImGui::GetItemID(), ImGuiInputFlags_LockThisFrame);
        const auto before = position;
        const auto pressed = [&](ImGuiKey key) { return ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, ImGui::GetItemID()); };
        if (pressed(ImGuiKey_LeftArrow)) position -= 5;
        if (pressed(ImGuiKey_RightArrow)) position += 5;
        if (pressed(ImGuiKey_Home)) position = 0;
        if (pressed(ImGuiKey_End)) position = duration;
        position = std::clamp(position, 0.0, duration); changed = changed || before != position;
    }
    return changed;
}

inline float filenameFontSize() {
    const auto glyph = ImGui::GetFontBaked()->FindGlyph('H');
    return text::active->fontSizeForCapHeight(glyph->Y1 - glyph->Y0);
}
inline void highlightedLabel(const std::string& label, const std::vector<std::wstring>& terms, ImVec2 origin) {
    const auto glyph = ImGui::GetFontBaked()->FindGlyph('H'); const auto size = filenameFontSize();
    origin.y += std::round(glyph->Y0 - (size * .8f - (glyph->Y1 - glyph->Y0)));
    text::active->draw(label, terms, origin, size, ImGui::GetColorU32(ImGuiCol_Text));
}
inline void filenameText(const std::string& label, float maximum = 0) {
    const auto origin = ImGui::GetCursorScreenPos();
    const auto width = text::active->width(label, filenameFontSize());
    ImGui::Dummy({maximum > 0 ? std::min(maximum, width) : width, ImGui::GetFontSize()});
    highlightedLabel(label, {}, origin);
}

inline void peakMeter(double peak, ImVec2 size, float rowHeight = 0) {
    const auto height = std::max(size.y, rowHeight);
    ImGui::Dummy({size.x, height});
    auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    min.y = std::round(min.y + (height - size.y) * .5f); max.y = min.y + size.y;
    auto draw = ImGui::GetWindowDrawList(); draw->AddRectFilled(min, max, IM_COL32(24, 29, 31, 255), 2);
    const auto db = peak > 0 ? 20 * std::log10(peak) : -60;
    const float level = static_cast<float>(std::clamp((db + 60) / 60, 0.0, 1.0));
    draw->AddRectFilled(min, {min.x + size.x * level, max.y}, peak >= 1 ? IM_COL32(245, 91, 83, 255) : IM_COL32(64, 224, 177, 255), 2);
}

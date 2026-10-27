#pragma once
#include "analysis.h"
#include "waveform_image.h"
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

inline bool waveformSeek(WaveformImage& image, const std::shared_ptr<const analysis::Result>& waveform, double& position, double duration, ImVec2 size) {
    ImGui::InvisibleButton("##waveform", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(24, 29, 31, 255), 3);
    bool changed = false;
    if (duration > 0 && ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        const auto target = duration * std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - min.x) / size.x), 0.0, 1.0);
        changed = ImGui::IsItemActivated() || (ImGui::GetIO().MouseDelta.x != 0 && std::abs(target-position) > .0001);
        position = target;
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
    image.draw(waveform,min,max,duration > 0 ? position/duration : 0);
    if (ImGui::IsItemFocused()) draw->AddRect(min,max,ImGui::GetColorU32(ImGuiCol_NavCursor),3);
    return changed;
}

inline float filenameFontSize() {
    const auto glyph = ImGui::GetFontBaked()->FindGlyph('H');
    return text::active->fontSizeForCapHeight(glyph->Y1 - glyph->Y0);
}
inline void highlightedLabel(const std::string& label, const std::vector<std::wstring>& terms, ImVec2 origin, float maximumWidth = 0) {
    const auto glyph = ImGui::GetFontBaked()->FindGlyph('H'); const auto size = filenameFontSize();
    origin.y += std::round(glyph->Y0 - (size * .8f - (glyph->Y1 - glyph->Y0)));
    text::active->draw(label, terms, origin, size, ImGui::GetColorU32(ImGuiCol_Text),maximumWidth);
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

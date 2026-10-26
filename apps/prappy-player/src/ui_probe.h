#pragma once
#include "state_io.h"
#include "imgui.h"
#include <iomanip>
#include <cmath>
#include <map>
#include <sstream>
#include <vector>

// Explicit --window-test instrumentation. Normal player sessions do no probe I/O.
// Tests wait for observed hover/press/activation instead of guessing frame delays.
struct UiProbe {
    bool enabled = false;
    bool scripted = false, mouseDown = false, rightDown = false;
    ImVec2 mousePosition{};
    uint64_t acknowledged = 0;
    float scrollX = 0, scrollY = 0, scrollMaxY = 0;
    int order = 0;
    std::map<ImGuiKey, bool> keys;
    std::vector<std::string> controls;
    std::map<std::string, uint64_t> activations;
    std::string tooltip, previous;
    std::string features;
    void input(const std::filesystem::path& directory) {
        if (!enabled) return;
        std::string characters;
        float wheel = 0;
        try {
            const auto bytes = stateio::read(directory / "ui-command.txt");
            if (!bytes.empty() && bytes.size() < 4096 && bytes.back() == '\n') {
                std::istringstream command(bytes); command.imbue(std::locale::classic());
                uint64_t sequence = 0; std::string action;
                if (command >> sequence >> action; sequence > acknowledged) {
                    bool valid = true;
                    if (action == "MOVE") {
                        float x = 0, y = 0;
                        valid = static_cast<bool>(command >> x >> y) && std::isfinite(x) && std::isfinite(y) && std::abs(x) < 100000 && std::abs(y) < 100000;
                        if (valid) mousePosition = {x, y};
                    } else if (action == "DOWN") mouseDown = true;
                    else if (action == "UP") mouseDown = false;
                    else if (action == "RIGHT_DOWN") rightDown = true;
                    else if (action == "RIGHT_UP") rightDown = false;
                    else if (action == "WHEEL") valid = static_cast<bool>(command >> wheel) && std::isfinite(wheel) && std::abs(wheel) <= 100;
                    else if (action == "KEY") {
                        std::string name; int down = 0;
                        valid = static_cast<bool>(command >> name >> down) && (down == 0 || down == 1);
                        const auto key = name == "HOME" ? ImGuiKey_Home : name == "DOWN" ? ImGuiKey_DownArrow : name == "ENTER" ? ImGuiKey_Enter :
                            name == "ESC" ? ImGuiKey_Escape : name == "LEFT" ? ImGuiKey_LeftArrow : name == "RIGHT" ? ImGuiKey_RightArrow :
                            name == "END" ? ImGuiKey_End : name == "CTRL" ? ImGuiMod_Ctrl : name == "SHIFT" ? ImGuiMod_Shift :
                            name == "A" ? ImGuiKey_A : name == "M" ? ImGuiKey_M : name == "T" ? ImGuiKey_T : ImGuiKey_None;
                        if (valid && key != ImGuiKey_None) keys[key] = down != 0; else valid = false;
                    }
                    else if (action == "TEXT") valid = static_cast<bool>(command >> std::quoted(characters));
                    else valid = false;
                    command >> std::ws;
                    if (valid && command.eof()) { acknowledged = sequence; scripted = true; }
                }
            }
        } catch (const std::exception&) { /* No command yet, or an incomplete write: read next frame. */ }
        if (scripted) {
            auto& io = ImGui::GetIO();
            io.ClearEventsQueue();
            io.ConfigInputTrickleEventQueue = false;
            io.AddFocusEvent(true);
            io.AddMousePosEvent(mousePosition.x, mousePosition.y);
            io.AddMouseButtonEvent(0, mouseDown);
            io.AddMouseButtonEvent(1, rightDown);
            for (const auto& [key, down] : keys) io.AddKeyEvent(key, down);
            if (wheel != 0) io.AddMouseWheelEvent(0, wheel);
            if (!characters.empty()) io.AddInputCharactersUTF8(characters.c_str());
        }
    }
    void begin() { if (enabled) { controls.clear(); tooltip.clear(); scrollX = scrollY = scrollMaxY = 0; } }
    void observe(const char* name, bool activated) {
        if (!enabled || !ImGui::IsItemVisible()) return;
        if (activated) ++activations[name];
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "CONTROL " << std::quoted(name) << ' ' << min.x << ' ' << min.y << ' ' << max.x << ' ' << max.y
            << ' ' << ImGui::IsItemHovered() << ' ' << ImGui::IsItemActive() << ' ' << activations[name] << '\n';
        controls.push_back(out.str());
    }
    void flush(const std::filesystem::path& directory, const std::string& state) {
        if (!enabled) return;
        std::ostringstream out; out << state << ' ' << acknowledged << '\n';
        out << "SCROLL " << scrollX << ' ' << scrollY << ' ' << scrollMaxY << '\n';
        out << "ORDER " << order << '\n';
        if (!features.empty()) out << features << '\n';
        for (const auto& [name, count] : activations) out << "ACTIVATION " << std::quoted(name) << ' ' << count << '\n';
        for (const auto& control : controls) out << control;
        auto text = tooltip; std::replace(text.begin(), text.end(), '\n', ' ');
        out << "TOOLTIP " << std::quoted(text) << '\n';
        const auto snapshot = out.str();
        if (snapshot == previous) return;
        try { stateio::write(directory / "ui-status.txt", snapshot, false); previous = snapshot; }
        catch (const std::exception& e) { OutputDebugStringA(e.what()); }
    }
};
static UiProbe uiProbe;

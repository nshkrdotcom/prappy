// UI setup follows Dear ImGui's MIT-licensed example_sdl3_sdlrenderer3.
#include "player_ui.h"
#include "analysis.h"
#include "playback.h"
#include "scan.h"
#include "status.h"
#include "tests.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <initguid.h>
#include <propkey.h>
#include <propsys.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <cmath>
#include <cstdio>
#include <utility>
namespace fs = std::filesystem;
constexpr int Width = 500;
constexpr float Scale = Width / 350.0f;
constexpr float u(float value) { return value * Scale; }
constexpr int MinHeight = 250, TransportHeight = 202;
static WNDPROC originalProc;
static constexpr wchar_t AppId[] = L"nshkrdotcom.Prappy.Player";
static std::vector<SDL_FRect> controls, nextControls;
#include "ui_probe.h"
static void rememberControl() {
    if (!ImGui::IsItemVisible()) return;
    auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const auto window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    min.x = std::max(min.x, window.x); min.y = std::max(min.y, window.y);
    max.x = std::min(max.x, window.x + size.x); max.y = std::min(max.y, window.y + size.y);
    nextControls.push_back({min.x, min.y, max.x - min.x, max.y - min.y});
}

static void setTaskbarIdentity(HWND hwnd) {
    IPropertyStore* properties = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&properties))))
        throw std::runtime_error("Cannot set player taskbar identity.");
    const auto release = [](IPropertyStore* value) { value->Release(); };
    const std::unique_ptr<IPropertyStore, decltype(release)> owner(properties, release);
    HRESULT result = S_OK;
    auto set = [&](REFPROPERTYKEY property, const std::wstring& text) {
        PROPVARIANT value{}; value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<wchar_t*>(text.c_str());
        if (SUCCEEDED(result)) result = properties->SetValue(property, value);
    };
    const auto directory = executableDir();
    const auto executable = (directory / L"PrappyPlayer.exe").wstring();
    // Pinning uses shell metadata, separately from the live WM_SETICON icon.
    // A dedicated icon path also avoids the EXE's old cached icon association.
    set(PKEY_AppUserModel_RelaunchCommand, L"\"" + executable + L"\"");
    set(PKEY_AppUserModel_RelaunchDisplayNameResource, L"@" + executable + L",-101");
    set(PKEY_AppUserModel_RelaunchIconResource, (directory / L"PrappyPlayer.ico").wstring() + L",0");
    set(PKEY_AppUserModel_ID, AppId);
    if (SUCCEEDED(result)) result = properties->Commit();
    if (FAILED(result)) throw std::runtime_error("Cannot set player taskbar properties.");
}
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCHITTEST) {
        // Leave the native frame/caption to Windows (including Snap Layouts).
        // SDL's hit test applies only to whitespace inside the client area.
        auto hit = DefWindowProcW(hwnd, message, w, l);
        if (hit != HTCLIENT) return hit;
    }
    return CallWindowProcW(originalProc, hwnd, message, w, l);
}
static SDL_HitTestResult SDLCALL hitTest(SDL_Window*, const SDL_Point* point, void*) {
    if (ImGui::GetCurrentContext() && (ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))) return SDL_HITTEST_NORMAL;
    for (const auto& rect : controls)
        if (point->x >= rect.x && point->x < rect.x + rect.w && point->y >= rect.y && point->y < rect.y + rect.h)
            return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}
static void tooltip(const std::string& text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text.c_str());
        if (uiProbe.enabled) uiProbe.tooltip = text;
    }
}
static void centeredLabel(const char* label, ImVec2 min, ImVec2 max, ImU32 color) {
    // Center horizontal ink, but share one cap-height baseline across labels.
    // Centering each word vertically moves descenders (History's y) off baseline.
    const char* end = label;
    float pen = 0;
    ImVec2 inkMin{ImGui::GetFontSize(), ImGui::GetFontSize()}, inkMax{};
    bool visible = false;
    auto font = ImGui::GetFontBaked();
    // Player button labels are ASCII; ## suffixes remain standard hidden IDs.
    for (; *end && !(end[0] == '#' && end[1] == '#'); ++end) {
        const auto glyph = font->FindGlyph(static_cast<unsigned char>(*end));
        if (glyph->Visible) {
            inkMin.x = std::min(inkMin.x, pen + glyph->X0); inkMin.y = std::min(inkMin.y, glyph->Y0);
            inkMax.x = std::max(inkMax.x, pen + glyph->X1); inkMax.y = std::max(inkMax.y, glyph->Y1);
            visible = true;
        }
        pen += glyph->AdvanceX;
    }
    if (visible) {
        const auto capital = font->FindGlyph('H');
        const ImVec2 pos{std::round((min.x + max.x - inkMin.x - inkMax.x) * .5f),
                         std::round((min.y + max.y - capital->Y0 - capital->Y1) * .5f)};
        auto draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        draw->AddText(pos, color, label, end);
        draw->PopClipRect();
    }
}
static bool centeredButton(const char* label, ImVec2 size = {}, const char* probeName = nullptr) {
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    uiProbe.observe(probeName ? probeName : label, pressed);
    if (ImGui::IsItemVisible()) centeredLabel(label, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), color);
    return pressed;
}
static bool navigationButton(const char* id, bool forward, ImVec2 size) {
    const bool pressed = ImGui::Button(id, size);
    uiProbe.observe(forward ? "next" : "back", pressed);
    if (!ImGui::IsItemVisible()) return pressed;
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 center{std::round((min.x + max.x) * .5f), std::round((min.y + max.y) * .5f)};
    constexpr float halfWidth = 6, halfHeight = 6;
    const float direction = forward ? 1.0f : -1.0f;
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    auto draw = ImGui::GetWindowDrawList();
    draw->AddTriangleFilled({center.x + direction * 3, center.y},
        {center.x - direction * halfWidth, center.y - halfHeight}, {center.x - direction * halfWidth, center.y + halfHeight}, color);
    draw->AddRectFilled({center.x + direction * 5 - 1, center.y - halfHeight},
        {center.x + direction * 5 + 1, center.y + halfHeight}, color);
    return pressed;
}
static bool listButton(bool expanded, ImVec2 size) {
    const bool pressed = ImGui::Button("##list", size);
    uiProbe.observe("list", pressed);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    const float textWidth = ImGui::CalcTextSize("List").x;
    const float groupLeft = std::round((min.x + max.x - textWidth - 15) * .5f);
    centeredLabel("List", {groupLeft, min.y}, {groupLeft + textWidth, max.y}, color);
    const ImVec2 center{groupLeft + textWidth + 11, std::round((min.y + max.y) * .5f)};
    const float direction = expanded ? -1.0f : 1.0f;
    const ImVec2 points[] = {{center.x - 4, center.y - direction * 2}, {center.x, center.y + direction * 2}, {center.x + 4, center.y - direction * 2}};
    ImGui::GetWindowDrawList()->AddPolyline(points, 3, color, ImDrawFlags_None, 1.5f);
    return pressed;
}
static bool repeatButton(RepeatMode mode, ImVec2 size) {
    const bool pressed = ImGui::Button("##repeat", size);
    uiProbe.observe("repeat", pressed);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 center{std::round((min.x + max.x) * .5f), std::round((min.y + max.y) * .5f)};
    constexpr float r = 8, h = 5, tip = 3;
    const auto color = ImGui::GetColorU32(mode == RepeatMode::Off ? ImVec4(.64f, .68f, .69f, 1) : ImVec4(.25f, .94f, .72f, 1));
    auto draw = ImGui::GetWindowDrawList();
    const ImVec2 upper[] = {{center.x-r, center.y}, {center.x-r, center.y-h}, {center.x+r, center.y-h}};
    const ImVec2 lower[] = {{center.x+r, center.y}, {center.x+r, center.y+h}, {center.x-r, center.y+h}};
    draw->AddPolyline(upper, 3, color, ImDrawFlags_None, 1.5f);
    draw->AddPolyline(lower, 3, color, ImDrawFlags_None, 1.5f);
    draw->AddTriangleFilled({center.x+r, center.y-h}, {center.x+r-tip, center.y-h-tip}, {center.x+r-tip, center.y-h+tip}, color);
    draw->AddTriangleFilled({center.x-r, center.y+h}, {center.x-r+tip, center.y+h-tip}, {center.x-r+tip, center.y+h+tip}, color);
    if (mode == RepeatMode::One) {
        draw->AddLine({center.x-2, center.y-1}, {center.x, center.y-2}, color, 1.5f);
        draw->AddLine({center.x, center.y-2}, {center.x, center.y+2}, color, 1.5f);
    }
    return pressed;
}
static bool viewButton(const char* label, bool selected) {
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.16f, .39f, .31f, 1));
    const bool pressed = centeredButton(label);
    if (selected) ImGui::PopStyleColor();
    return pressed;
}
static std::string historyTime(int64_t timestamp) {
    const time_t value = static_cast<time_t>(timestamp); tm local{}; char text[32]{};
    if (localtime_s(&local, &value) == 0) std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}
static std::string timeText(double seconds) {
    int n = static_cast<int>(std::max(0.0, seconds));
    char buf[32]; std::snprintf(buf, sizeof(buf), "%d:%02d", n / 60, n % 60); return buf;
}
static void style() {
    ImGui::StyleColorsDark(); auto& s = ImGui::GetStyle();
    s.WindowPadding = {8, 5}; s.FramePadding = {5, 3};
    s.ItemSpacing = {5, 4}; s.ItemInnerSpacing = {4, 3}; s.ScrollbarSize = 9;
    s.WindowBorderSize = 0; s.FrameRounding = 3; s.GrabRounding = 3;
    s.Colors[ImGuiCol_WindowBg] = {0.18f, .19f, .20f, 1};
    s.Colors[ImGuiCol_ChildBg] = {.155f, .165f, .175f, 1};
    s.Colors[ImGuiCol_Button] = {.26f, .28f, .30f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {.20f, .43f, .37f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {.16f, .58f, .43f, 1};
    s.Colors[ImGuiCol_SliderGrab] = {.25f, .94f, .72f, 1};
    s.Colors[ImGuiCol_SliderGrabActive] = {.50f, 1, .83f, 1};
    s.Colors[ImGuiCol_CheckMark] = {.25f, .94f, .72f, 1};
    s.Colors[ImGuiCol_Header] = {.16f, .39f, .31f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {.19f, .48f, .38f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {.20f, .57f, .43f, 1};
    s.ScaleAllSizes(Scale); s.FontSizeBase = 18;
}
int runPlayer(HINSTANCE instance) {
    struct ComLifetime {
        HRESULT result = E_FAIL;
        ~ComLifetime() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    // SDL timers/TLS can be used by self-tests before video is initialized.
    // Keep shutdown outside both paths and after their local objects unwind.
    struct SdlLifetime { ~SdlLifetime() { SDL_Quit(); } } sdlLifetime;
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("Cannot read player command line.");
    const std::unique_ptr<void, decltype(&LocalFree)> arguments(argv, LocalFree);
    std::vector<std::wstring> args; for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (args.size() == 3 && args[0] == L"--self-test") return selfTest(args[1], args[2]);
    if (args.size() == 3 && args[0] == L"--state-test") return stateTests(args[1], args[2]);
    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
    bool imgui = false, platform = false, graphics = false;
    int exitCode = 0;
    std::unique_ptr<void, decltype(&CloseHandle)> singleton(nullptr, CloseHandle);
    try {
        const bool windowTest = !args.empty() && args[0] == L"--window-test";
        uiProbe.enabled = windowTest;
        if (!windowTest) {
            HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PrappyPlayer.Instance");
            const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
            if (!mutex) throw std::runtime_error("Cannot create player instance lock.");
            singleton.reset(mutex);
            if (alreadyRunning) {
                if (auto existing = FindWindowW(nullptr, L"Prappy Player")) {
                    ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing);
                }
                return 0;
            }
        }
        com.result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com.result)) throw std::runtime_error("Cannot initialize Windows COM services.");
        SetCurrentProcessExplicitAppUserModelID(AppId);
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        const auto stateDirectory = windowTest ? (args.size() == 2 ? fs::absolute(args[1]) : executableDir() / "window-test") : executableDir();
        fs::create_directories(stateDirectory);
        const auto config = stateDirectory / "player.ini";
        Library library; Audio audio;
        Scan scan;
        try { createDefaultConfig(config); } catch (const std::exception& e) { library.notice = e.what(); }
        Config settings;
        try {
            settings = readConfig(config);
            loadCatalog(stateDirectory / "player-catalog.bin", settings, library);
        } catch (const std::exception& e) { library.notice = e.what(); }
        audio.open(windowTest);
        HistoryFile historyFile(stateDirectory / "player-history.txt");
        auto history = historyFile.load(settings.historyLimit);
        Playback playback(audio, library, history, historyFile);
        analysis::Worker analyzer(stateDirectory / "player-analysis");
        std::map<std::wstring, analysis::Summary> analyses;
        uint64_t analysisGeneration = 0;
        bool forceAnalysis = false;
        std::wstring currentAnalysisId;
        const auto prioritizeCurrent = [&] {
            const auto id = key(audio.current);
            const auto track = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& t) { return Library::matches(t, id); });
            currentAnalysisId = track == library.tracks.end() ? id : track->id;
            if (track != library.tracks.end()) analyzer.prioritize(*track);
        };
        playback.restore();
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Prappy Player", Width, MinHeight,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(window, Width, MinHeight);
        auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        setTaskbarIdentity(hwnd);
        originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        const BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkCaption, sizeof(darkCaption));
        auto icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SDL_SetWindowHitTest(window, hitTest, nullptr);
        // A small desktop UI does not need a game graphics device/overlay hook.
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui = true;
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        style();
        const fs::path font = L"C:\\Windows\\Fonts\\segoeui.ttf";
        if (fs::exists(font)) io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), 18);
        platform = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        graphics = ImGui_ImplSDLRenderer3_Init(renderer);
        if (!platform || !graphics) throw std::runtime_error("Cannot initialize UI renderer.");
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(window);
        scan.begin(config, library.order, &library, true);
        bool done = false, expanded = false;
        bool historyView = false;
        uint64_t historyGeneration = 0;
        char search[256]{};
        bool filterDirty = true, resetListScroll = true;
        std::vector<size_t> visible;
        int expandedHeight = 710;
        const auto togglePlayback = [&] { playback.toggle(); };
        while (!done) {
            const auto start = SDL_GetTicks();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (!uiProbe.scripted && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                    event.key.key == SDLK_SPACE && !io.WantTextInput &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) togglePlayback();
                    continue;
                }
                if (!uiProbe.scripted) ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
            }
            playback.tick();
            if (scan.update(library)) {
                history.setLimit(library.historyLimit); filterDirty = true;
                analyses.clear(); analyzer.reconcile(library, forceAnalysis); forceAnalysis = false;
                prioritizeCurrent();
            }
            for (auto& result : analyzer.collect()) {
                const auto id = key(result.identity.path);
                const auto track = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& t) { return t.id == id; });
                if (track != library.tracks.end() && track->bytes == result.identity.bytes && track->stamp == result.identity.stamp)
                    analyses.insert_or_assign(id, std::move(result));
            }
            if (analysisGeneration != playback.generation) {
                analysisGeneration = playback.generation;
                prioritizeCurrent();
            }
            if (historyGeneration != playback.generation) {
                historyGeneration = playback.generation;
                if (historyView) filterDirty = true;
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(80); continue; }
            ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame();
            uiProbe.input(stateDirectory);
            ImGui::NewFrame();
            uiProbe.begin();
            nextControls.clear();
            int width, height; SDL_GetWindowSize(window, &width, &height);
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
            ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            const auto& uiStyle = ImGui::GetStyle();
            const float headerY = std::round(uiStyle.WindowPadding.y);
            constexpr float headerHeight = 26, headerWidth = 74, gap = 7;
            const float headerRight = std::round(ImGui::GetWindowContentRegionMax().x);
            const auto titleMin = ImGui::GetCursorScreenPos();
            centeredLabel("PRAPPY  /  PLAYER", titleMin, {titleMin.x + 116, titleMin.y + headerHeight},
                ImGui::GetColorU32(ImVec4(.25f, .94f, .72f, 1)));
            ImGui::SetCursorPos({headerRight - headerWidth * 2 - gap, headerY});
            if (centeredButton("History", {headerWidth, headerHeight}, "history")) {
                historyView = true; filterDirty = resetListScroll = true;
                if (!expanded && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) SDL_SetWindowSize(window, width, expandedHeight);
                expanded = true;
            }
            rememberControl(); tooltip("Browse listening history");
            ImGui::SetCursorPos({headerRight - headerWidth, headerY});
            if (listButton(expanded, {headerWidth, headerHeight})) {
                if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
                    if (expanded) { expandedHeight = height; SDL_SetWindowSize(window, width, MinHeight); }
                    else SDL_SetWindowSize(window, width, expandedHeight);
                }
                expanded = !expanded;
            }
            rememberControl();
            tooltip(expanded ? "Collapse list" : "Expand list");
            ImGui::SetCursorPos({std::round(uiStyle.WindowPadding.x), headerY + headerHeight + std::round(uiStyle.ItemSpacing.y)});
            const float transportY = static_cast<float>(height - TransportHeight);
            if (expanded && transportY > u(90)) {
                if (viewButton("Library", !historyView)) { historyView = false; filterDirty = resetListScroll = true; }
                rememberControl(); ImGui::SameLine();
                if (viewButton("History##tab", historyView)) { historyView = true; filterDirty = resetListScroll = true; }
                rememberControl(); ImGui::SameLine();
                if (centeredButton("Config")) {
                    auto result = ShellExecuteW(hwnd, L"open", config.c_str(), nullptr, config.parent_path().c_str(), SW_SHOWNORMAL);
                    if (reinterpret_cast<INT_PTR>(result) <= 32) library.notice = "Could not open player.ini.";
                }
                rememberControl();
                ImGui::SetNextItemWidth(u(153));
                const char* choices[] = {"Filename", "Directory groups", "File type groups"};
                ImGui::BeginDisabled(historyView);
                if (ImGui::Combo("##order", &library.order, choices, 3)) { library.sort(); filterDirty = resetListScroll = true; }
                uiProbe.observe("order", ImGui::IsItemClicked());
                rememberControl(); ImGui::EndDisabled();
                if (historyView) { ImGui::SameLine(); ImGui::TextDisabled("Newest first"); }
                const auto listStart = ImGui::GetCursorPos();
                const float searchY = transportY - ImGui::GetFrameHeight() - uiStyle.ItemSpacing.y - u(4);
                ImGui::SetCursorPos({listStart.x, searchY});
                const float clearWidth = ImGui::CalcTextSize("Clear").x + uiStyle.FramePadding.x * 2;
                const float countWidth = ImGui::CalcTextSize("00000 / 00000").x;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - countWidth - uiStyle.ItemSpacing.x * 2);
                if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputTextWithHint("##search", "Search names or paths...", search, sizeof(search), ImGuiInputTextFlags_EscapeClearsAll)) filterDirty = resetListScroll = true;
                uiProbe.observe("search", ImGui::IsItemClicked());
                rememberControl();
                tooltip("Match all words, in any order, ignoring case. Ctrl+F focuses search; Escape clears it.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!search[0]);
                if (centeredButton("Clear")) { search[0] = '\0'; filterDirty = resetListScroll = true; }
                rememberControl(); ImGui::EndDisabled();
                // Playback/history updates may rebuild rows without moving the viewport.
                // Only explicit changes to the user's view request a scroll reset.
                const bool resetScroll = resetListScroll;
                resetListScroll = false;
                if (filterDirty) {
                    if (!historyView) visible = library.filter(search);
                    else {
                        visible.clear();
                        std::wistringstream input(searchText(search)); std::vector<std::wstring> terms;
                        for (std::wstring term; input >> term;) terms.push_back(std::move(term));
                        for (size_t i = history.entries.size(); i > 0; --i) {
                            const auto& text = std::as_const(history.entries)[i - 1].search;
                            if (std::all_of(terms.begin(), terms.end(), [&](const auto& term) { return text.find(term) != std::wstring::npos; })) visible.push_back(i - 1);
                        }
                    }
                    filterDirty = false;
                }
                ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", visible.size(), historyView ? history.entries.size() : library.tracks.size());
                ImGui::SetCursorPos(listStart);
                if (ImGui::BeginChild("Tracks", {0, std::max(1.0f, searchY - listStart.y - uiStyle.ItemSpacing.y)}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
                    if (uiProbe.enabled) {
                        uiProbe.scrollX = ImGui::GetScrollX(); uiProbe.scrollY = ImGui::GetScrollY(); uiProbe.scrollMaxY = ImGui::GetScrollMaxY();
                    }
                    if (resetScroll) { ImGui::SetScrollY(0); ImGui::SetScrollX(0); }
                    const auto current = key(audio.current);
                    auto row = [&](size_t i) {
                        if (historyView) {
                            const auto& entry = std::as_const(history.entries)[i];
                            const bool unavailable = playback.unavailable.contains(key(entry.path));
                            const auto label = (unavailable ? "[Unavailable] " : "") + library.pathLabel(entry.path) + "  \xc2\xb7  " + historyTime(entry.time);
                            ImGui::PushID(static_cast<int>(i));
                            const bool selected = ImGui::Selectable(label.c_str(), history.current() && history.current()->id == entry.id);
                            uiProbe.observe(("history-" + std::to_string(entry.id)).c_str(), selected);
                            if (selected) playback.request(entry.path);
                            rememberControl(); tooltip(utf8(entry.path) + "\nPlayed: " + historyTime(entry.time));
                            ImGui::PopID(); return;
                        }
                        const auto& t = library.tracks[i];
                        ImGui::PushID(static_cast<int>(i));
                        const auto label = (t.offline ? "[Offline] " : "") + t.label;
                        const bool selected = ImGui::Selectable(label.c_str(), Library::matches(t, current));
                        uiProbe.observe(("library-" + std::to_string(i)).c_str(), selected);
                        if (selected) playback.request(t.path);
                        rememberControl();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            std::string paths = t.label;
                            if (!t.copies.empty()) {
                                paths += "\n\nIdentical copies (" + std::to_string(t.copies.size()) + "):";
                                for (auto& copy : t.copies) paths += "\n" + library.pathLabel(copy);
                            }
                            ImGui::BeginTooltip();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 44);
                            ImGui::TextUnformatted(paths.c_str());
                            ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                        }
                        ImGui::PopID();
                    };
                    if (historyView || library.order == 0) {
                        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(visible.size()));
                        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(visible[static_cast<size_t>(i)]);
                    } else {
                        size_t begin = 0;
                        while (begin < visible.size()) {
                            const auto& first = library.tracks[visible[begin]];
                            const auto group = library.order == 1 ? first.directory : first.type;
                            size_t end = begin + 1;
                            while (end < visible.size() && (library.order == 1 ? library.tracks[visible[end]].directory : library.tracks[visible[end]].type) == group) ++end;
                            ImGui::PushID(group.c_str());
                            const bool open = ImGui::CollapsingHeader((group + " (" + std::to_string(end - begin) + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                            rememberControl();
                            tooltip(group);
                            if (open) {
                                ImGuiListClipper clipper; clipper.Begin(static_cast<int>(end - begin));
                                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(visible[begin + static_cast<size_t>(i)]);
                            }
                            ImGui::PopID(); begin = end;
                        }
                    }
                    if (historyView && history.entries.empty()) ImGui::TextWrapped("Tracks appear here after they successfully load.");
                    else if (!historyView && library.tracks.empty()) ImGui::TextWrapped("Add music roots in Config, then Refresh.");
                    else if (visible.empty()) ImGui::TextDisabled("No matches.");
                    if (ImGui::GetScrollMaxY() > 0) {
                        auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                        const auto bar = ImGui::GetStyle().ScrollbarSize;
                        nextControls.push_back({pos.x + size.x - bar, pos.y, bar, size.y});
                    }
                    if (ImGui::GetScrollMaxX() > 0) {
                        auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                        const auto bar = ImGui::GetStyle().ScrollbarSize;
                        nextControls.push_back({pos.x, pos.y + size.y - bar, size.x, bar});
                    }
                }
                ImGui::EndChild();
            }
            auto draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled({0, transportY}, {static_cast<float>(width), static_cast<float>(height)}, IM_COL32(49, 53, 56, 255));
            ImGui::SetCursorPos({u(8), transportY + u(4)});
            ImGui::TextUnformatted(audio.current.empty() ? "Choose a track from List" : library.pathLabel(audio.current).c_str());
            uiProbe.observe("audio-info", false);
            std::string details = library.pathLabel(audio.current);
            if (audio.loaded) {
                details += "\n" + audio.info.describe();
                const auto analyzed = analyses.find(currentAnalysisId);
                if (analyzed != analyses.end()) {
                    const auto& result = analyzed->second;
                    if (!result.error.empty()) details += "\nAnalysis: " + result.error;
                    else {
                        details += "\nStereo analysis: " + timeText(result.duration);
                        if (result.loudness) {
                            char value[64]; std::snprintf(value, sizeof(value), " · %.1f LUFS", *result.loudness); details += value;
                        } else details += " · loudness unavailable (silence or under 400 ms)";
                    }
                } else details += "\nStereo analysis pending";
            }
            tooltip(details);
            ImGui::SetNextItemWidth(static_cast<float>(width) - u(16));
            float seek = static_cast<float>(audio.position), duration = static_cast<float>(audio.duration);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, {.96f, .74f, .39f, 1});
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {1, .84f, .55f, 1});
            ImGui::BeginDisabled(!audio.loaded || duration <= 0);
            if (ImGui::SliderFloat("##seek", &seek, 0, std::max(duration, .01f), "")) audio.seek(seek);
            rememberControl();
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::TextDisabled("%s / %s", timeText(audio.position).c_str(), timeText(audio.duration).c_str());
            ImGui::SameLine(u(248)); ImGui::TextDisabled("%s", audio.active ? (audio.paused ? "PAUSED" : "PLAYING") : "READY");
            const float transportRowY = std::round(ImGui::GetCursorPosY());
            constexpr float buttonHeight = 34, arrowWidth = 46, playWidth = 80, repeatWidth = 34, refreshWidth = 92;
            const float transportLeft = std::round(uiStyle.WindowPadding.x);
            const float transportRight = static_cast<float>(width) - transportLeft;
            const float playX = transportLeft + arrowWidth + gap, nextX = playX + playWidth + gap;
            const float refreshX = transportRight - refreshWidth, repeatX = refreshX - gap - repeatWidth;
            const float valueX = nextX + arrowWidth + gap, sliderX = valueX + 46 + gap;
            ImGui::SetCursorPos({transportLeft, transportRowY});
            ImGui::BeginDisabled(!playback.adjacent(-1));
            if (navigationButton("##back", false, {arrowWidth, buttonHeight})) playback.navigate(-1);
            rememberControl();
            ImGui::EndDisabled();
            tooltip("Back: " + library.pathLabel(playback.destination(-1)));
            ImGui::SetCursorPos({playX, transportRowY});
            ImGui::PushStyleColor(ImGuiCol_Button, {.96f, .74f, .39f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, .84f, .55f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.84f, .59f, .24f, 1});
            ImGui::PushStyleColor(ImGuiCol_Text, {.12f, .15f, .16f, 1});
            const bool wantsPause = playback.pending ? playback.pending->paused : audio.paused;
            if (centeredButton(audio.active && !wantsPause ? "Pause" : "Play", {playWidth, buttonHeight}, "play")) togglePlayback();
            rememberControl();
            ImGui::PopStyleColor(4);
            ImGui::SetCursorPos({nextX, transportRowY});
            if (navigationButton("##next", true, {arrowWidth, buttonHeight})) playback.navigate(1);
            rememberControl();
            tooltip(std::string(playback.adjacent(1) ? "Forward: " : "Next in library: ") + library.pathLabel(playback.destination(1)));
            ImGui::SetCursorPos({valueX, transportRowY});
            ImGui::Dummy({46, buttonHeight});
            const auto volumeLabel = "Vol " + std::to_string(static_cast<int>(std::round(audio.volume)));
            centeredLabel(volumeLabel.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
            uiProbe.observe("volume-value", false);
            ImGui::SetCursorPos({sliderX, transportRowY + std::round((buttonHeight - ImGui::GetFrameHeight()) * .5f)});
            ImGui::SetNextItemWidth(repeatX - gap - sliderX);
            // Preserve whole-point mouse/keyboard adjustments with an external readout.
            int volume = static_cast<int>(std::round(audio.volume));
            if (ImGui::SliderInt("##volume", &volume, 0, 100, "")) audio.setVolume(volume);
            uiProbe.observe("volume", ImGui::IsItemClicked());
            rememberControl(); tooltip("Volume: " + std::to_string(static_cast<int>(std::round(audio.volume))) + "%");
            ImGui::SetCursorPos({repeatX, transportRowY});
            if (repeatButton(history.repeat, {repeatWidth, buttonHeight})) { history.cycleRepeat(); historyFile.save(history, true); }
            rememberControl(); tooltip(repeatTooltip(history.repeat));
            ImGui::SetCursorPos({refreshX, transportRowY});
            ImGui::BeginDisabled(scan.running());
            if (centeredButton(scan.running() ? "Scanning" : "Refresh", {refreshWidth, buttonHeight}, "refresh")) {
                forceAnalysis = ImGui::GetIO().KeyShift;
                if (ImGui::GetIO().KeyShift) { library.verified.clear(); library.forceVerification = true; }
                scan.begin(config, library.order, &library, true);
            }
            rememberControl();
            ImGui::EndDisabled();
            tooltip("Refresh the catalog. Hold Shift to reverify duplicate contents and rebuild audio analysis.");
            ImGui::SetCursorPosY(static_cast<float>(height) - u(24));
            ImGui::PushStyleColor(ImGuiCol_Text, audio.error.empty() ? ImVec4(.68f, .72f, .72f, 1) : ImVec4(1, .63f, .5f, 1));
            const auto message = !historyFile.notice.empty() ? historyFile.notice : (!playback.notice.empty() ? playback.notice :
                (!audio.error.empty() ? audio.error : (scan.running() ? statusText(library.notice, scan.status()) : library.notice)));
            const auto displayMessage = statusText(message);
            ImGui::TextUnformatted(displayMessage.c_str()); tooltip(displayMessage); ImGui::PopStyleColor();
            draw->AddRect({0, 0}, {static_cast<float>(width - 1), static_cast<float>(height - 1)}, IM_COL32(90, 99, 99, 255));
            ImGui::End(); controls.swap(nextControls); ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 46, 49, 51, 255); SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
            if (windowTest) {
                uiProbe.order = library.order;
                std::ostringstream status;
                status << "READY " << width << ' ' << height << ' ' << static_cast<int>(history.repeat) << ' '
                    << (history.current() ? history.current()->id : 0) << ' ' << history.entries.size() << ' '
                    << playback.pending.has_value() << ' ' << audio.paused << ' ' << historyView << ' ' << visible.size();
                uiProbe.flush(stateDirectory, status.str());
            }
            const auto elapsed = SDL_GetTicks() - start;
            if (elapsed < 33) SDL_Delay(static_cast<Uint32>(33 - elapsed));
        }
        playback.finish();
        if (historyFile.writable && history.revision != historyFile.saved && !historyFile.notice.empty())
            MessageBoxA(hwnd, historyFile.notice.c_str(), "Prappy Player: history was not saved", MB_OK | MB_ICONWARNING);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR); exitCode = 1;
    }
    if (graphics) ImGui_ImplSDLRenderer3_Shutdown();
    if (platform) ImGui_ImplSDL3_Shutdown();
    if (imgui) ImGui::DestroyContext();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    return exitCode;
}

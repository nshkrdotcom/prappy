// UI setup follows Dear ImGui's MIT-licensed example_sdl3_sdlrenderer3.
#include "player_ui.h"
#include "analysis.h"
#include "playback.h"
#include "scan.h"
#include "status.h"
#include "tests.h"
#include "feature_ui.h"
#include "preferences.h"
#include "ui_wake.h"
#include "ui_schedule.h"
#include "grouped_list.h"
#include "directory_ui.h"
#include "presentation.h"
#include "sample_workspace.h"
#include "player_sample_ui.h"
#include "sample_tests.h"
#include "implot.h"
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
constexpr int MinHeight = 250;
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
        ImGui::BeginTooltip();
        std::istringstream lines(text); for (std::string line; std::getline(lines, line);)
            filenameText(line, std::max(100.0f, ImGui::GetIO().DisplaySize.x - 32));
        ImGui::EndTooltip();
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
    if (args.size() == 3 && args[0] == L"--meter-test") return outputMeterTests(args[1], args[2]);
    if (args.size() == 3 && args[0] == L"--sampling-test") return sampling::integrationTest(args[1],args[2]);
    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
    Presentation presentation;
    bool imgui = false, platform = false, graphics = false;
    int exitCode = 0;
    std::unique_ptr<void, decltype(&CloseHandle)> singleton(nullptr, CloseHandle);
    try {
        const bool windowTest = !args.empty() && args[0] == L"--window-test";
        uiProbe.enabled = windowTest;
        const auto testInstance = windowTest ? SDL_GetEnvironmentVariable(SDL_GetEnvironment(),"PRAPPY_TEST_INSTANCE") : nullptr;
        if (!windowTest || testInstance) {
            const auto mutexName = testInstance ? fs::path(testInstance).wstring() : L"Local\\PrappyPlayer.Instance";
            HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
            const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
            if (!mutex) throw std::runtime_error("Cannot create player instance lock.");
            singleton.reset(mutex);
            if (alreadyRunning) {
                if (auto existing = !windowTest ? FindWindowW(nullptr, L"Prappy Player") : nullptr) {
                    ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing);
                }
                return 0;
            }
        }
        com.result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com.result)) throw std::runtime_error("Cannot initialize Windows COM services.");
        SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "none");
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        const auto wake = std::make_shared<UiWake>();
        const auto notify = [wake] { wake->notify(); };
        SetCurrentProcessExplicitAppUserModelID(AppId);
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        const auto stateDirectory = windowTest ? (args.size() == 2 ? fs::absolute(args[1]) : executableDir() / "window-test") : executableDir();
        fs::create_directories(stateDirectory);
        const auto config = stateDirectory / "player.ini";
        auto preferences = Preferences::load(stateDirectory / "player-ui.txt");
        stateio::Writer preferenceWriter;
        preferenceWriter.notify = notify;
        uint64_t preferenceRevision = 0, savedPreferences = 0;
        std::chrono::steady_clock::time_point nextPreferenceSave{};
        Library library; Audio audio;
        audio.volume = preferences.volume;
        Scan scan;
        scan.notify = notify;
        try { createDefaultConfig(config); } catch (const std::exception& e) { library.notice = e.what(); }
        Config settings;
        try {
            settings = readConfig(config);
            loadCatalog(stateDirectory / "player-catalog.bin", settings, library);
        } catch (const std::exception& e) { library.notice = e.what(); }
        audio.open(windowTest);
        audio.setNotify([wake] { wake->notify(UiWake::Audio); });
        HistoryFile historyFile(stateDirectory / "player-history.txt");
        historyFile.writer.notify = notify;
        auto history = historyFile.load(settings.historyLimit);
        Playback playback(audio, library, history, historyFile);
        playback.folder = preferences.folder;
        analysis::Worker analyzer(stateDirectory / "player-analysis", notify);
        std::map<std::wstring, analysis::Summary> analyses;
        uint64_t analysisGeneration = 0;
        bool forceAnalysis = false;
        bool durationSortDirty = false;
        std::wstring currentAnalysisId;
        std::wstring waveformId;
        std::shared_ptr<const analysis::Result> waveform;
        Browse browser;
        browser.view = static_cast<Browse::View>(preferences.session.view);
        browser.availability = preferences.session.availability;
        browser.extension = preferences.session.extension;
        browser.minimum = preferences.session.minimum; browser.maximum = preferences.session.maximum;
        browser.measuredOnly = preferences.session.measuredOnly;
        library.order = preferences.session.order; library.sort();
        for (const auto& path : preferences.session.includedDirectories) DirectoryFilter::set(browser.directories.included, path, true);
        for (const auto& path : preferences.hiddenDirectories) DirectoryFilter::set(browser.directories.excluded, path, true);
        DirectoryIndex directoryIndex; directoryIndex.rebuild(library); directoryIndex.prioritize(preferences.pinnedDirectories);
        DirectoryUi directoryUi;
        directoryUi.location = preferences.session.directoryLocation.wstring();
        std::copy(preferences.session.directorySearch.begin(), preferences.session.directorySearch.end(), directoryUi.search);
        GroupedList grouped;
        grouped.closed = preferences.session.closedGroups;
        uint64_t browseVisits = UINT64_MAX;
        bool browseCatalogDirty = true;
        const auto gainForPath = [&](const fs::path& path) {
            const auto id = key(path);
            const auto track = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& t) { return Library::matches(t, id); });
            const auto result = analyses.find(track == library.tracks.end() ? id : track->id);
            return normalizationGain(result == analyses.end() ? nullptr : &result->second, preferences.normalize);
        };
        playback.prepare = [&](const fs::path& path) { audio.setGain(gainForPath(path)); };
        const auto prioritizeCurrent = [&] {
            const auto id = key(audio.current);
            const auto track = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& t) { return Library::matches(t, id); });
            currentAnalysisId = track == library.tracks.end() ? id : track->id;
            if (waveformId != currentAnalysisId) { waveform.reset(); waveformId = currentAnalysisId; }
            if (track != library.tracks.end()) analyzer.prioritize(*track);
        };
        playback.restore();
        constexpr int StripHeight = 86;
        window = SDL_CreateWindow("Prappy Player", preferences.session.width, preferences.strip ? StripHeight : std::max(MinHeight,preferences.session.height),
            SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        uiProbe.windowID = SDL_GetWindowID(window);
        SDL_SetWindowMinimumSize(window, Width, preferences.strip ? StripHeight : MinHeight);
        SDL_SetWindowAlwaysOnTop(window, preferences.topmost);
        auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        setTaskbarIdentity(hwnd);
        originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        const BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkCaption, sizeof(darkCaption));
        auto icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SDL_SetWindowHitTest(window, hitTest, nullptr);
        // Software comparison is available only in isolated test windows.
        renderer = presentation.open(window,windowTest && SDL_GetEnvironmentVariable(SDL_GetEnvironment(),"PRAPPY_TEST_SOFTWARE") != nullptr);
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui = true;
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        style();
        text::loadFonts();
        WaveformView waveformImage(renderer);
        text::Painter textPainter(renderer); text::active = &textPainter;
        sampling::Workspace sampleWorkspace(stateDirectory,notify);
        PlayerSampleSelection waveformSelection;
        platform = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        graphics = ImGui_ImplSDLRenderer3_Init(renderer);
        if (!platform || !graphics) throw std::runtime_error("Cannot initialize UI renderer.");
        if (preferences.session.positioned) {
            auto& s = preferences.session;
            RECT rectangle{s.x,s.y,s.x+s.width,s.y+s.height};
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromRect(&rectangle,MONITOR_DEFAULTTONEAREST),&monitor)) {
                const auto& work = monitor.rcWork;
                s.width = std::min(s.width,static_cast<int>(std::max<LONG>(Width,work.right-work.left)));
                s.height = std::min(s.height,static_cast<int>(std::max<LONG>(MinHeight,work.bottom-work.top)));
                SDL_SetWindowSize(window,s.width,preferences.strip ? StripHeight : std::max(MinHeight,s.height));
                s.x = std::clamp(s.x,static_cast<int>(work.left),static_cast<int>(std::max(work.left,work.right-s.width)));
                s.y = std::clamp(s.y,static_cast<int>(work.top),static_cast<int>(std::max(work.top,work.bottom-std::min(s.height,86))));
            }
            SDL_SetWindowPosition(window,s.x,s.y);
        } else SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (preferences.session.maximized && !preferences.strip) SDL_MaximizeWindow(window);
        SDL_ShowWindow(window);
        scan.begin(config, library.order, &library, true);
        bool done = false, expanded = preferences.session.expanded;
        bool historyView = preferences.session.history;
        uint64_t historyGeneration = 0;
        char search[256]{};
        std::copy(preferences.session.search.begin(),preferences.session.search.end(),search);
        bool restoreScroll = true;
        float savedScrollX = preferences.session.scrollX, savedScrollY = preferences.session.scrollY;
        bool filterDirty = true, resetListScroll = true;
        std::vector<size_t> visible;
        int expandedHeight = preferences.session.expandedHeight;
        int normalHeight = preferences.session.normalHeight;
        bool normalMaximized = preferences.session.normalMaximized;
        double displayedPeak = 0;
        fs::path contextPath;
        float transportDragY = 0; int transportDragHeight = 0;
        std::optional<SDL_FRect> transportGrip;
        bool transportResetRequested = false, transportResetMouseUp = false;
        const auto persistDirectories = [&] {
            preferences.hiddenDirectories = browser.directories.exclusions(); ++preferenceRevision;
        };
        const auto setStrip = [&](bool enabled) {
            int width = 0, height = 0; SDL_GetWindowSize(window, &width, &height);
            if (enabled) {
                normalHeight = height; normalMaximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
                if (normalMaximized) SDL_RestoreWindow(window);
            }
            preferences.strip = enabled; ++preferenceRevision;
            SDL_SetWindowMinimumSize(window, Width, enabled ? StripHeight : MinHeight);
            SDL_SetWindowSize(window, width, enabled ? StripHeight : std::max(MinHeight, normalHeight));
            if (!enabled && normalMaximized) SDL_MaximizeWindow(window);
        };
        const auto menuItem = [&](const char* label, const char* probe, bool enabled = true) {
            const bool chosen = ImGui::MenuItem(label, nullptr, false, enabled); uiProbe.observe(probe, chosen); return chosen;
        };
        const auto pathMenu = [&](const fs::path& path) {
            if (menuItem("Copy full path", "copy-path")) {
                if (!SDL_SetClipboardText(utf8(displayPath(path)).c_str())) library.notice = SDL_GetError();
            }
            const auto linux = wslLinuxPath(path);
            if (menuItem("Copy WSL Linux path", "copy-wsl", linux.has_value())) {
                if (!SDL_SetClipboardText(linux->c_str())) library.notice = SDL_GetError();
            }
            if (menuItem("Reveal in Explorer", "reveal")) if (!revealFile(hwnd, path)) library.notice = "Cannot reveal this file in Explorer.";
            ImGui::Separator();
            if (menuItem("Show only this folder", "directory-only-folder")) { browser.directories.only(path.parent_path()); filterDirty = resetListScroll = true; }
            if (menuItem("Include this folder", "directory-add-folder")) { DirectoryFilter::set(browser.directories.included, path.parent_path(), true); filterDirty = resetListScroll = true; }
            if (menuItem("Hide this folder and subfolders", "directory-hide-folder")) {
                DirectoryFilter::set(browser.directories.excluded, path.parent_path(), true); persistDirectories(); filterDirty = resetListScroll = true;
            }
            ImGui::Separator();
            if (menuItem("Play next", "queue-next")) playback.enqueue(path, true);
            if (menuItem("Add to queue", "queue-add")) playback.enqueue(path, false);
            ImGui::Separator();
            if (menuItem("Play this folder", "play-folder")) {
                if (playback.playFolder(path.parent_path(), false)) { preferences.folder = playback.folder; ++preferenceRevision; }
            }
            if (menuItem("Repeat this folder", "repeat-folder")) {
                if (playback.playFolder(path.parent_path(), true)) { preferences.folder = playback.folder; ++preferenceRevision; }
            }
        };
        const auto moreMenu = [&] {
            if (ImGui::BeginPopup("Options")) {
                if (menuItem("Open current song in sample editor (Ctrl+M)", "sampling",audio.loaded)) { sampleWorkspace.mark(audio); ImGui::CloseCurrentPopup(); }
                if (ImGui::Checkbox("Always on top", &preferences.topmost)) {
                    SDL_SetWindowAlwaysOnTop(window, preferences.topmost); ++preferenceRevision;
                }
                uiProbe.observe("topmost", ImGui::IsItemActivated());
                bool strip = preferences.strip;
                if (ImGui::Checkbox("Slim strip", &strip)) { setStrip(strip); ImGui::CloseCurrentPopup(); }
                uiProbe.observe("strip", ImGui::IsItemActivated());
                if (ImGui::Checkbox("Normalize to -18 LUFS", &preferences.normalize)) {
                    ++preferenceRevision; if (!preferences.normalize) audio.setGain(0);
                }
                uiProbe.observe("normalize", ImGui::IsItemActivated());
                ImGui::TextDisabled("Measured gain, at most +12 dB; -1 dB sample-peak ceiling");
                if (preferences.normalize) ImGui::Text("Applied gain: %+.1f dB", audio.gain);
                if (!playback.folder.empty()) {
                    ImGui::TextWrapped("Folder: %s", utf8(playback.folder).c_str());
                    if (menuItem("Play all folders", "clear-folder")) { playback.folder.clear(); preferences.folder.clear(); ++preferenceRevision; }
                }
                ImGui::SeparatorText("Upcoming in navigation trail");
                uint64_t remove = 0;
                const auto first = history.current() ? history.cursor + 1 : 0;
                if (first < history.trail.size() && ImGui::BeginChild("Upcoming", {440, 150}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
                    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
                    ImGuiListClipper clipper; clipper.Begin(static_cast<int>(history.trail.size() - first), rowHeight);
                    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const auto entry = history.find(history.trail[first + static_cast<size_t>(row)]); if (!entry) continue;
                        const auto startY = ImGui::GetCursorPosY();
                        ImGui::PushID(static_cast<int>(entry->id));
                        if (entry->queued && ImGui::SmallButton("Remove")) remove = entry->id;
                        if (entry->queued) uiProbe.observe(("queue-remove-" + std::to_string(entry->id)).c_str(), ImGui::IsItemActivated());
                        if (entry->queued) ImGui::SameLine();
                        filenameText((entry->queued ? "Queued: " : "Forward: ") + library.pathLabel(entry->path));
                        tooltip(utf8(entry->path)); ImGui::PopID();
                        const float gap = rowHeight - (ImGui::GetCursorPosY() - startY);
                        if (gap > 0) ImGui::Dummy({0, std::max(0.0f, gap - ImGui::GetStyle().ItemSpacing.y)});
                    }
                    ImGui::EndChild();
                } else if (first < history.trail.size()) {
                    ImGui::EndChild();
                }
                if (remove) playback.removeQueued(remove);
                if (first == history.trail.size()) ImGui::TextDisabled("Nothing queued. Right-click a track to add it.");
                ImGui::EndPopup();
            }
        };
        const auto togglePlayback = [&] { playback.toggle(); };
        UiSchedule schedule;
        auto lastUpdate = UiSchedule::Clock::now();
        bool gainMoving = false, mouseHeld = false, composing = false;
        std::set<SDL_Keycode> heldKeys, imeKeys;
        Uint64 imeUntil = 0;
        uint64_t probeSequence = 0;
        size_t scanFound = 0, scanChecked = 0;
        Uint64 inputTimestamp = 0;
        bool samplingWasOpen=false;
        int listeningWidth=Width,listeningHeight=MinHeight;
        while (!done) {
            sampleWorkspace.tick(audio);
            if (sampleWorkspace.active()!=samplingWasOpen) {
                if (sampleWorkspace.active()) { SDL_GetWindowSize(window,&listeningWidth,&listeningHeight); SDL_SetWindowSize(window,std::max(1100,listeningWidth),std::max(850,listeningHeight)); }
                else SDL_SetWindowSize(window,listeningWidth,listeningHeight);
                samplingWasOpen=sampleWorkspace.active();
            }
            auto now = UiSchedule::Clock::now();
            const auto flags = SDL_GetWindowFlags(window);
            const bool visibleWindow = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0;
            const bool playing = (audio.active && !audio.paused) || sampleWorkspace.animating();
            const bool probeHeld = uiProbe.scripted && (uiProbe.mouseDown || uiProbe.rightDown ||
                std::any_of(uiProbe.keys.begin(), uiProbe.keys.end(), [](const auto& value) { return value.second; }));
            const bool animate = (visibleWindow && (playing || mouseHeld || !heldKeys.empty() || probeHeld || displayedPeak > 0)) || gainMoving;
            auto persistence = UiSchedule::Time::max();
            if (history.revision != historyFile.saved && !historyFile.writer.busy() && historyFile.writable)
                persistence = historyFile.urgent && historyFile.writer.error.empty() ? now : historyFile.nextSave;
            if (preferenceRevision != savedPreferences && !preferenceWriter.busy()) persistence = std::min(persistence, nextPreferenceSave);
            SDL_Event event{};
            bool received = SDL_WaitEventTimeout(&event, schedule.wait(now, visibleWindow, animate, windowTest && uiProbe.pollCommands, scan.running(), persistence));
            if (received) ++schedule.wakeups;
            do {
                if (!received) break;
                now = UiSchedule::Clock::now();
                if (event.type == SDL_EVENT_RENDER_DEVICE_LOST) throw std::runtime_error("The graphics device was lost and could not be recovered.");
                if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                    textPainter.resetTextures(); waveformImage.resetTexture();
                    ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
                    presentation.configureLatency(); schedule.request(3);
                }
                if (event.type == wake->eventType()) { if (wake->acknowledge() & UiWake::Work) schedule.request(2); }
                else { schedule.input(now); if (!inputTimestamp && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL || event.type == SDL_EVENT_KEY_DOWN)) inputTimestamp = event.common.timestamp; }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouseHeld = true;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) mouseHeld = SDL_GetMouseState(nullptr, nullptr) != 0;
                if (!preferences.strip && transportGrip && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    event.button.windowID == SDL_GetWindowID(window) && event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2 &&
                    event.button.x >= transportGrip->x && event.button.x < transportGrip->x+transportGrip->w &&
                    event.button.y >= transportGrip->y && event.button.y < transportGrip->y+transportGrip->h &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                    transportResetRequested = transportResetMouseUp = true; schedule.request(2); continue;
                }
                if (transportResetMouseUp && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) { transportResetMouseUp = false; continue; }
                if (event.type == SDL_EVENT_KEY_DOWN) heldKeys.insert(event.key.key);
                if (event.type == SDL_EVENT_KEY_UP) heldKeys.erase(event.key.key);
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) { heldKeys.clear(); imeKeys.clear(); mouseHeld = false; composing = false; transportResetMouseUp = false; }
                if (event.type == SDL_EVENT_TEXT_EDITING) { composing = event.edit.text && event.edit.text[0]; imeUntil = SDL_GetTicks() + 100; }
                if (event.type == SDL_EVENT_TEXT_INPUT && composing) imeUntil = SDL_GetTicks() + 100;
                const bool imeKey = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                    (event.key.key == SDLK_RETURN || event.key.key == SDLK_ESCAPE || event.key.key == SDLK_SPACE);
                if (imeKey && event.type == SDL_EVENT_KEY_DOWN && (composing || SDL_GetTicks() < imeUntil)) imeKeys.insert(event.key.key);
                if (imeKey && imeKeys.contains(event.key.key)) { if (event.type == SDL_EVENT_KEY_UP) imeKeys.erase(event.key.key); continue; }
                if (!uiProbe.scripted && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                    event.key.key == SDLK_SPACE && !io.WantTextInput && !composing &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) { if(sampleWorkspace.active())sampleWorkspace.toggleTransport(audio);else togglePlayback(); }
                    continue;
                }
                if (!uiProbe.scripted) ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
            } while ((received = SDL_PollEvent(&event)));
            if (done) break;
            now = UiSchedule::Clock::now();
            const double elapsed = std::chrono::duration<double>(now - lastUpdate).count(); lastUpdate = now;
            const auto previousGeneration = playback.generation;
            const auto previousPause = audio.paused; const auto previousActive = audio.active;
            const auto previousPosition = audio.position, previousDuration = audio.duration;
            const auto previousSeekPreview = audio.seekPreview;
            playback.tick();
            if (previousSeekPreview != audio.seekPreview) schedule.request();
            if (previousGeneration != playback.generation || previousPause != audio.paused || previousActive != audio.active) schedule.request(2);
            if (audio.paused && (previousPosition != audio.position || previousDuration != audio.duration)) schedule.request();
            if (scan.update(library)) {
                schedule.request(2);
                directoryIndex.rebuild(library); directoryIndex.prioritize(preferences.pinnedDirectories);
                history.setLimit(library.historyLimit); filterDirty = true;
                analyses.clear(); analyzer.reconcile(library, forceAnalysis); forceAnalysis = false;
                waveform.reset(); browseVisits = UINT64_MAX; browseCatalogDirty = true;
                prioritizeCurrent();
            }
            for (auto& result : analyzer.collect()) {
                schedule.request(2);
                const auto id = key(result.identity.path);
                const auto track = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& t) { return t.id == id; });
                if (track != library.tracks.end() && track->bytes == result.identity.bytes && track->stamp == result.identity.stamp) {
                    if (result.waveform && id == currentAnalysisId) { waveform = result.waveform; waveformId = id; }
                    result.waveform.reset();
                    if (result.error.empty()) track->duration = result.duration;
                    analyses.insert_or_assign(id, std::move(result));
                    if (!historyView && (browser.minimum > 0 || browser.maximum > 0 || browser.measuredOnly)) filterDirty = true;
                    if (library.order == 3) durationSortDirty = true;
                }
            }
            if (durationSortDirty && library.order == 3 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                library.sort(); durationSortDirty = false; filterDirty = true;
            }
            if (browseVisits != history.visitsRevision) {
                browseVisits = history.visitsRevision; browser.rebuild(library, history, browseCatalogDirty); browseCatalogDirty = false; filterDirty = true;
            }
            if (analysisGeneration != playback.generation) {
                analysisGeneration = playback.generation; prioritizeCurrent();
            }
            if (audio.loaded) {
                const auto found = analyses.find(currentAnalysisId);
                const auto target = normalizationGain(found == analyses.end() ? nullptr : &found->second, preferences.normalize);
                const auto nextGain = approachGain(audio.gain, target, elapsed);
                gainMoving = std::abs(target - audio.gain) > .001;
                if (std::abs(nextGain - audio.gain) > .001) audio.setGain(nextGain);
            }
            else gainMoving = false;
            displayedPeak = releasePeak(displayedPeak, audio.meterPeak, elapsed);
            if (preferenceWriter.collect()) {
                if (!preferenceWriter.error.empty()) { library.notice = preferenceWriter.error; nextPreferenceSave = std::chrono::steady_clock::now() + std::chrono::seconds(5); }
                else savedPreferences = preferenceWriter.revision;
            }
            if (!preferenceWriter.busy() && preferenceRevision != savedPreferences && std::chrono::steady_clock::now() >= nextPreferenceSave) {
                const auto bytes = preferences.serialize();
                preferenceWriter.startGenerated(stateDirectory / "player-ui.txt", [bytes] { return bytes; }, preferenceRevision);
            }
            if (historyGeneration != playback.generation) {
                historyGeneration = playback.generation;
                if (historyView) filterDirty = true;
            }
            if (scan.running()) {
                const auto found = scan.progress.found.load(), checked = scan.progress.checked.load();
                if (found != scanFound || checked != scanChecked) { scanFound = found; scanChecked = checked; schedule.request(); }
            }
            if (windowTest) {
                uiProbe.input(stateDirectory, false);
                if (probeSequence != uiProbe.acknowledged) { probeSequence = uiProbe.acknowledged; schedule.input(now); if (!inputTimestamp) inputTimestamp = SDL_GetTicksNS(); }
            }
            if (!visibleWindow) { schedule.paint = now + std::chrono::milliseconds(33); continue; }
            const bool animateNow = sampleWorkspace.animating() || (audio.active && !audio.paused) || mouseHeld || !heldKeys.empty() || probeHeld || displayedPeak > 0 || gainMoving;
            if (!schedule.due(now, animateNow)) continue;
            const auto frameStart = Presentation::Clock::now();
            textPainter.beginFrame();
            ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame();
            // Scripted input is reapplied after the platform backend queues mouse state.
            if (windowTest) uiProbe.input(stateDirectory);
            ImGui::NewFrame();
            uiProbe.begin();
            nextControls.clear();
            waveformSelection.sync(utf8(audio.current),audio.loadedSerial);
            if (!ImGui::GetIO().WantTextInput&&ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_M, ImGuiInputFlags_RouteGlobal)) sampleWorkspace.mark(audio);
            if(!sampleWorkspace.active()&&audio.loaded&&!ImGui::GetIO().WantTextInput){
                if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_E,ImGuiInputFlags_RouteGlobal)){waveformSelection.begin(audio.displayPosition(),audio.duration);if(preferences.strip)setStrip(false);}
                if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiMod_Alt|ImGuiKey_M,ImGuiInputFlags_RouteGlobal))sampleWorkspace.mark(audio,{},sampling::OpenSource::Samples);
                if(waveformSelection.active&&!ImGui::GetIO().WantTextInput){
                    if(ImGui::Shortcut(ImGuiKey_Escape,ImGuiInputFlags_RouteGlobal))waveformSelection.cancel();
                    if(ImGui::Shortcut(ImGuiKey_Enter,ImGuiInputFlags_RouteGlobal)){sampleWorkspace.mark(audio,sampling::SecondsSpan{waveformSelection.start,waveformSelection.end});waveformSelection.active=false;}
                }
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, ImGuiInputFlags_RouteGlobal)) setStrip(!preferences.strip);
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T, ImGuiInputFlags_RouteGlobal)) {
                preferences.topmost = !preferences.topmost; ++preferenceRevision; SDL_SetWindowAlwaysOnTop(window, preferences.topmost);
            }
            int width, height; SDL_GetWindowSize(window, &width, &height);
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
            ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            const auto drawPlayerWaveform=[&](ImVec2 size){
                double seek=audio.displayPosition();
                if(waveformSeek(waveformImage,waveformId==currentAnalysisId?waveform:nullptr,seek,audio.duration,size,!waveformSelection.active))audio.seek(seek);
                const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();uiProbe.observe("seek",ImGui::IsItemActivated());rememberControl();
                waveformSelection.draw(min,max,audio.displayPosition(),audio.duration,preferences.strip,
                    [&](std::optional<sampling::SecondsSpan> span,sampling::OpenSource action){sampleWorkspace.mark(audio,span,action);waveformSelection.active=false;},
                    [&](const char* id,bool hit){uiProbe.observe(id,hit);rememberControl();});
                if(preferences.strip&&waveformSelection.active)setStrip(false);
            };
            if (preferences.strip) {
                transportGrip.reset();
                transportResetRequested = false;
                ImGui::SetCursorPos({10, 5});
                if (centeredButton("<##strip", {30, 26}, "back")) playback.navigate(-1); rememberControl();
                ImGui::SameLine();
                if (centeredButton(audio.active && !audio.paused ? "Pause##strip" : "Play##strip", {55, 26}, "play")) togglePlayback(); rememberControl();
                ImGui::SameLine();
                if (centeredButton(">##strip", {30, 26}, "next")) playback.navigate(1); rememberControl();
                ImGui::SameLine();
                if (centeredButton("More##strip", {58, 26}, "more")) ImGui::OpenPopup("Options"); rememberControl();
                ImGui::SameLine(); filenameText(audio.current.empty() ? "Choose a track" : utf8(audio.current.filename()));
                if (!audio.current.empty()) rememberControl();
                tooltip(library.pathLabel(audio.current));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) contextPath = audio.current;
                if (ImGui::BeginPopupContextItem("strip-track")) { if (!contextPath.empty()) pathMenu(contextPath); ImGui::EndPopup(); }
                moreMenu();
                ImGui::SetCursorPos({10, 37});
                ImGui::BeginDisabled(!audio.loaded || audio.duration <= 0);
                drawPlayerWaveform({static_cast<float>(width)-20,26});ImGui::EndDisabled();
                ImGui::SetCursorPos({10, 69}); peakMeter(displayedPeak, {static_cast<float>(width) - 20, 4});
                tooltip(audio.loaded && !audio.paused && !audio.meterAvailable ? "Waiting for Windows audio-session meter" : "Prappy Windows audio-session peak after volume/gain; not a true-peak meter");
            } else {
            const auto& uiStyle = ImGui::GetStyle();
            const float headerY = std::round(uiStyle.WindowPadding.y);
            constexpr float headerHeight = 26, headerWidth = 74, gap = 7;
            const float headerRight = std::round(ImGui::GetWindowContentRegionMax().x);
            const bool browserVisible = expanded && height-TransportLayout::height(preferences.transportHeight,height,true) > 190;
            const auto titleMin = ImGui::GetCursorScreenPos();
            centeredLabel("PRAPPY  /  PLAYER", titleMin, {titleMin.x + 116, titleMin.y + headerHeight},
                ImGui::GetColorU32(ImVec4(.25f, .94f, .72f, 1)));
            ImGui::SetCursorPos({156, headerY});
            if (centeredButton("More", {58, headerHeight}, "more")) ImGui::OpenPopup("Options"); rememberControl(); moreMenu();
            ImGui::SetCursorPos({headerRight - headerWidth * 2 - gap, headerY});
            if (centeredButton("History", {headerWidth, headerHeight}, "history")) {
                historyView = true; filterDirty = resetListScroll = true;
                if (!browserVisible && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) SDL_SetWindowSize(window, width, std::max(452,expandedHeight));
                expanded = true;
            }
            rememberControl(); tooltip("Browse listening history");
            ImGui::SetCursorPos({headerRight - headerWidth, headerY});
            if (listButton(browserVisible, {headerWidth, headerHeight})) {
                if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
                    if (browserVisible) { expandedHeight = height; SDL_SetWindowSize(window, width, MinHeight); }
                    else SDL_SetWindowSize(window, width, std::max(452,expandedHeight));
                }
                expanded = !browserVisible;
            }
            rememberControl();
            tooltip(expanded ? "Collapse list" : "Expand list");
            ImGui::SetCursorPos({std::round(uiStyle.WindowPadding.x), headerY + headerHeight + std::round(uiStyle.ItemSpacing.y)});
            auto panelHeight = TransportLayout::height(preferences.transportHeight, height, expanded);
            auto transportY = static_cast<float>(height - panelHeight);
            const auto toolbarPosition = ImGui::GetCursorPos();
            ImGui::SetCursorPos({8, transportY - 6});
            ImGui::InvisibleButton("##transport-resize", {static_cast<float>(width)-16, 6}, ImGuiButtonFlags_EnableNav);
            const auto gripMin = ImGui::GetItemRectMin(), gripMax = ImGui::GetItemRectMax();
            transportGrip = SDL_FRect{gripMin.x,gripMin.y,gripMax.x-gripMin.x,gripMax.y-gripMin.y};
            uiProbe.observe("transport-resize", ImGui::IsItemActivated()); rememberControl();
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActivated()) { transportDragY = io.MousePos.y; transportDragHeight = panelHeight; }
            int preferred = preferences.transportHeight;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) preferred = TransportLayout::resized(transportDragHeight, transportDragY-io.MousePos.y, height, expanded);
            // Let the old grip claim input before the list expands into its position.
            if (transportResetRequested || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))) preferred = TransportLayout::Compact;
            transportResetRequested = false;
            if (ImGui::IsItemFocused()) {
                for (const auto key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Home}) ImGui::SetKeyOwner(key, ImGui::GetItemID(), ImGuiInputFlags_LockThisFrame);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) preferred = TransportLayout::height(panelHeight+16,height,expanded);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) preferred = TransportLayout::height(panelHeight-16,height,expanded);
                if (ImGui::IsKeyPressed(ImGuiKey_Home)) preferred = TransportLayout::Compact;
            }
            if (preferred != preferences.transportHeight) { preferences.transportHeight = preferred; ++preferenceRevision; nextPreferenceSave = now + std::chrono::milliseconds(300); schedule.request(2); }
            panelHeight = TransportLayout::height(preferences.transportHeight,height,expanded); transportY = static_cast<float>(height-panelHeight);
            transportGrip = SDL_FRect{8,transportY-6,static_cast<float>(width)-16,6};
            nextControls.push_back(*transportGrip);
            ImGui::GetWindowDrawList()->AddLine({static_cast<float>(width)/2-24,transportY-3},{static_cast<float>(width)/2+24,transportY-3},ImGui::GetColorU32(ImGuiCol_Separator),2);
            tooltip("Drag to resize the waveform. Double-click or press Home to reset; Up/Down adjusts height.");
            ImGui::SetCursorPos(toolbarPosition);
            if (expanded && transportY > 190) {
                if (viewButton("Library", !historyView)) { historyView = false; filterDirty = resetListScroll = true; }
                rememberControl(); ImGui::SameLine();
                if (viewButton("History##tab", historyView)) { historyView = true; filterDirty = resetListScroll = true; }
                rememberControl(); ImGui::SameLine();
                if (centeredButton("Config")) {
                    auto result = ShellExecuteW(hwnd, L"open", config.c_str(), nullptr, config.parent_path().c_str(), SW_SHOWNORMAL);
                    if (reinterpret_cast<INT_PTR>(result) <= 32) library.notice = "Could not open player.ini.";
                }
                rememberControl();
                ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                int view = static_cast<int>(browser.view);
                const char* views[] = {"All recordings", "Unplayed", "Most played", "Last played"};
                if (ImGui::Combo("##view", &view, views, 4)) { browser.view = static_cast<Browse::View>(view); historyView = false; filterDirty = resetListScroll = true; }
                uiProbe.observe("view", ImGui::IsItemClicked()); rememberControl();
                ImGui::SetNextItemWidth(u(153));
                const char* choices[] = {"Filename", "Directory groups", "File type groups", "Duration (shortest first)"};
                ImGui::BeginDisabled(historyView);
                if (ImGui::Combo("##order", &library.order, choices, 4)) { library.sort(); filterDirty = resetListScroll = true; }
                uiProbe.observe("order", ImGui::IsItemClicked());
                rememberControl(); ImGui::EndDisabled();
                if (historyView) { ImGui::SameLine(); ImGui::TextDisabled("Newest at bottom"); }
                else {
                    ImGui::SameLine();
                    if (centeredButton("Filters", {}, "filters")) ImGui::OpenPopup("Filters"); rememberControl();
                    if (ImGui::BeginPopup("Filters")) {
                        bool changed = false;
                        const char* availability[] = {"All availability", "Online only", "Offline only"};
                        ImGui::TextUnformatted("Availability"); ImGui::SetNextItemWidth(280);
                        changed |= ImGui::Combo("##availability", &browser.availability, availability, 3); uiProbe.observe("availability", ImGui::IsItemClicked());
                        ImGui::TextUnformatted("Extension"); ImGui::SetNextItemWidth(280);
                        if (ImGui::BeginCombo("##extension", browser.extension.empty() ? "All types" : browser.extension.c_str())) {
                            if (ImGui::Selectable("All types", browser.extension.empty())) { browser.extension.clear(); changed = true; }
                            for (const auto& extension : library.extensions) {
                                const auto type = extension.substr(1);
                                if (ImGui::Selectable(type.c_str(), browser.extension == type)) { browser.extension = type; changed = true; }
                            }
                            ImGui::EndCombo();
                        }
                        uiProbe.observe("extension", ImGui::IsItemClicked());
                        ImGui::TextUnformatted("Minimum duration (seconds)"); ImGui::SetNextItemWidth(280);
                        changed |= ImGui::InputDouble("##min-seconds", &browser.minimum, 10, 60, "%.0f"); uiProbe.observe("duration-min", ImGui::IsItemClicked());
                        ImGui::TextUnformatted("Maximum duration (0 = any)"); ImGui::SetNextItemWidth(280);
                        changed |= ImGui::InputDouble("##max-seconds", &browser.maximum, 10, 60, "%.0f"); uiProbe.observe("duration-max", ImGui::IsItemClicked());
                        browser.minimum = std::isfinite(browser.minimum) ? std::clamp(browser.minimum, 0.0, 28800.0) : 0;
                        browser.maximum = std::isfinite(browser.maximum) ? std::clamp(browser.maximum, 0.0, 28800.0) : 0;
                        changed |= ImGui::Checkbox("Measured duration only", &browser.measuredOnly); uiProbe.observe("measured-only", ImGui::IsItemActivated());
                        ImGui::TextDisabled("Duration bounds exclude tracks still awaiting analysis.");
                        if (ImGui::Button("Reset filters")) {
                            browser.availability = 0; browser.extension.clear(); browser.minimum = browser.maximum = 0; browser.measuredOnly = false; changed = true;
                        }
                        uiProbe.observe("filter-reset", ImGui::IsItemActivated());
                        if (changed) filterDirty = resetListScroll = true;
                        ImGui::EndPopup();
                    }
                }
                const auto directoryChange = directoryUi.draw(directoryIndex, browser.directories, preferences,
                    [&](const char* name, bool activated) { uiProbe.observe(name,activated); }, [&] { rememberControl(); });
                if (directoryChange.filter) filterDirty = resetListScroll = true;
                if (directoryChange.persistent) persistDirectories();
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
                    if (!historyView) visible = browser.select(library, search);
                    else {
                        visible.clear();
                        std::wistringstream input(searchText(search)); std::vector<std::wstring> terms;
                        for (std::wstring term; input >> term;) terms.push_back(std::move(term));
                        for (size_t i = history.entries.size(); i > 0; --i) {
                            const auto& text = std::as_const(history.entries)[i - 1].search;
                            if (!std::as_const(history.entries)[i - 1].queued && browser.directories.accepts(std::as_const(history.entries)[i - 1].path) && std::all_of(terms.begin(), terms.end(), [&](const auto& term) { return text.find(term) != std::wstring::npos; })) visible.push_back(i - 1);
                        }
                        std::stable_sort(visible.begin(), visible.end(), [&](size_t a, size_t b) {
                            const auto& entries = std::as_const(history.entries);
                            return entries[a].time != entries[b].time ? entries[a].time < entries[b].time : entries[a].id < entries[b].id;
                        });
                    }
                    if (!historyView && (library.order == 1 || library.order == 2)) grouped.rebuild(library, visible, [&](const Track& track) {
                        return library.order == 2 ? track.type : browser.directories.active() ? library.pathLabel(browser.directories.playbackPath(track).parent_path()) : track.directory;
                    });
                    filterDirty = false;
                }
                ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", visible.size(), historyView ? history.playedCount : library.tracks.size());
                ImGui::SetCursorPos(listStart);
                if (ImGui::BeginChild("Tracks", {0, std::max(1.0f, searchY - listStart.y - uiStyle.ItemSpacing.y)}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
                    if (restoreScroll && !scan.running()) {
                        ImGui::SetScrollX(preferences.session.scrollX); ImGui::SetScrollY(preferences.session.scrollY);
                        restoreScroll = false;
                    } else if (resetScroll && !restoreScroll) { ImGui::SetScrollY(0); ImGui::SetScrollX(0); }
                    if (!restoreScroll && !resetScroll) {
                        savedScrollX = ImGui::GetScrollX(); savedScrollY = ImGui::GetScrollY();
                    }
                    if (uiProbe.enabled) {
                        uiProbe.scrollX = ImGui::GetScrollX(); uiProbe.scrollY = ImGui::GetScrollY(); uiProbe.scrollMaxY = ImGui::GetScrollMaxY();
                    }
                    const auto current = key(audio.current);
                    const auto highlightTerms = Browse::tokens(search);
                    uiProbe.drawnTracks = 0;
                    auto row = [&](size_t i) {
                        ++uiProbe.drawnTracks;
                        if (historyView) {
                            const auto& entry = std::as_const(history.entries)[i];
                            const bool unavailable = playback.unavailable.contains(key(entry.path));
                            const auto label = (unavailable ? "[Unavailable] " : "") + library.pathLabel(entry.path) + "  \xc2\xb7  " + historyTime(entry.time);
                            ImGui::PushID(std::to_string(entry.id).c_str());
                            const auto origin = ImGui::GetCursorScreenPos();
                            const bool selected = ImGui::Selectable("##visit", history.current() && history.current()->id == entry.id, 0,
                                {std::max(ImGui::GetContentRegionAvail().x, textPainter.width(label, filenameFontSize()) + 4), 0});
                            highlightedLabel(label, highlightTerms, origin);
                            uiProbe.observe(("history-" + std::to_string(entry.id)).c_str(), selected);
                            if (selected) playback.selectHistory(entry.id,library.addNewListeningHistoryEntryWhenReplayingHistoryItem);
                            rememberControl(); tooltip(utf8(entry.path) + "\nPlayed: " + historyTime(entry.time));
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) contextPath = entry.path;
                            if (ImGui::BeginPopupContextItem("history-actions")) { pathMenu(contextPath); ImGui::EndPopup(); }
                            ImGui::PopID(); return;
                        }
                        const auto& t = library.tracks[i];
                        const auto playPath = browser.directories.active() ? browser.directories.playbackPath(t) : t.path;
                        ImGui::PushID(utf8(t.path).c_str());
                        std::string label = (t.offline ? "[Offline] " : "") + (playPath == t.path ? t.label : library.pathLabel(playPath));
                        const auto activity = browser.played(t);
                        if (browser.view == Browse::View::MostPlayed) label += " · " + std::to_string(activity.count) + " plays";
                        if (browser.view == Browse::View::LastPlayed) label += " · " + historyTime(activity.last);
                        if (t.duration) label += " · " + timeText(*t.duration);
                        const auto origin = ImGui::GetCursorScreenPos();
                        const bool selected = ImGui::Selectable("##track", Library::matches(t, current), ImGuiSelectableFlags_None,
                            {std::max(ImGui::GetContentRegionAvail().x, textPainter.width(label, filenameFontSize()) + 4), 0});
                        highlightedLabel(label, highlightTerms, origin);
                        uiProbe.observe(("library-" + std::to_string(i)).c_str(), selected);
                        if (selected) playback.request(playPath);
                        rememberControl();
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) contextPath = playPath;
                        if (ImGui::BeginPopupContextItem("track-actions")) { pathMenu(contextPath); ImGui::EndPopup(); }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            std::string paths = t.label;
                            if (!t.copies.empty()) {
                                paths += "\n\nIdentical copies (" + std::to_string(t.copies.size()) + "):";
                                for (auto& copy : t.copies) paths += "\n" + library.pathLabel(copy);
                            }
                            ImGui::BeginTooltip();
                            std::istringstream lines(paths); for (std::string line; std::getline(lines, line);) filenameText(line, io.DisplaySize.x - 32);
                            ImGui::EndTooltip();
                        }
                        ImGui::PopID();
                    };
                    if (historyView || library.order == 0 || library.order == 3 || search[0] || browser.view != Browse::View::All) {
                        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(visible.size()));
                        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(visible[static_cast<size_t>(i)]);
                    } else {
                        grouped.flatten(visible);
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(grouped.rows.size()), ImGui::GetTextLineHeightWithSpacing());
                        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                            const auto item = grouped.rows[static_cast<size_t>(i)];
                            if (!item.header) { row(item.index); continue; }
                            const auto& group = grouped.groups[item.index];
                            ImGui::PushID(group.key.c_str());
                            ImGui::SetNextItemOpen(group.open, ImGuiCond_Always);
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {uiStyle.FramePadding.x, 0});
                            const bool open = ImGui::CollapsingHeader(group.label.c_str());
                            ImGui::PopStyleVar();
                            uiProbe.observe(("group-" + std::to_string(item.index)).c_str(), open != group.open);
                            rememberControl(); tooltip(group.key);
                            if (open != group.open) { grouped.toggle(item.index, open); schedule.request(2); }
                            ImGui::PopID();
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
            ImGui::SetCursorPos({u(8), transportY + u(2)});
            const auto message = !historyFile.notice.empty() ? historyFile.notice : (!playback.notice.empty() ? playback.notice :
                (!audio.error.empty() ? audio.error : (scan.running() ? statusText(library.notice, scan.status()) : library.notice)));
            const auto displayMessage = statusText(message, playback.folder.empty() ? "" : "Folder: " + library.pathLabel(playback.folder));
            if(!displayMessage.empty()){
                ImGui::PushStyleColor(ImGuiCol_Text,audio.error.empty()?ImVec4(.68f,.72f,.72f,1):ImVec4(1,.63f,.5f,1));
                filenameText(displayMessage,static_cast<float>(width)*.45f);ImGui::PopStyleColor();tooltip(displayMessage);uiProbe.observe("footer",false);ImGui::SameLine();
            }
            filenameText(audio.current.empty() ? "Choose a track from List" : library.pathLabel(audio.current),std::max(10.f,static_cast<float>(width)-ImGui::GetCursorPosX()-u(8)));
            uiProbe.observe("audio-info", false);
            if (!audio.current.empty()) rememberControl();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) contextPath = audio.current;
            if (ImGui::BeginPopupContextItem("current-actions")) { if (!contextPath.empty()) pathMenu(contextPath); ImGui::EndPopup(); }
            std::string details = library.pathLabel(audio.current);
            if (audio.loaded) {
                details += "\n" + audio.info.describe();
                if (preferences.normalize) {
                    char gain[64]; std::snprintf(gain, sizeof(gain), "\nNormalization: %+.1f dB", audio.gain); details += gain;
                }
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
            constexpr float buttonHeight = 34, arrowWidth = 46, playWidth = 80, repeatWidth = 34, refreshWidth = 92;
            const float transportRowY=static_cast<float>(height)-std::round(uiStyle.WindowPadding.x)-buttonHeight;
            const float selectionHeight=waveformSelection.active?ImGui::GetTextLineHeight()*2+ImGui::GetFrameHeight()+uiStyle.ItemSpacing.y*3:ImGui::GetFrameHeight()+uiStyle.ItemSpacing.y;
            const float waveformHeight=transportRowY-ImGui::GetCursorPosY()-selectionHeight-ImGui::GetTextLineHeight()-uiStyle.ItemSpacing.y*2;
            ImGui::BeginDisabled(!audio.loaded || audio.duration <= 0);
            drawPlayerWaveform({static_cast<float>(width)-u(16),std::max(8.f,waveformHeight)});
            if(!waveformSelection.active){const bool edit=ImGui::Button("Edit samples (Ctrl+M)");uiProbe.observe("edit-samples",edit);rememberControl();if(edit)sampleWorkspace.mark(audio);if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",sampleWorkspace.preparationStatus().c_str());}
            ImGui::EndDisabled();
            tooltip(waveform ? "Stereo waveform scaled to the track peak. Drag to seek; focus and use Left/Right (5s), Home or End." : "Waveform analysis pending. Drag to seek.");
            ImGui::TextDisabled("%s / %s", timeText(audio.displayPosition()).c_str(), timeText(audio.duration).c_str());
            ImGui::SameLine(u(248)); ImGui::TextDisabled("%s", audio.active ? (audio.paused ? "PAUSED" : "PLAYING") : "READY");
            uiProbe.observe("transport-status", false);
            ImGui::SameLine(); peakMeter(displayedPeak, {std::max(30.0f, static_cast<float>(width) - ImGui::GetCursorPosX() - 12), 6}, ImGui::GetTextLineHeight());
            uiProbe.observe("peak-meter", false); tooltip(audio.loaded && !audio.paused && !audio.meterAvailable ? "Waiting for Windows audio-session meter" : "Prappy Windows audio-session peak after volume/gain; not a true-peak meter");
            const float transportLeft = std::round(uiStyle.WindowPadding.x);
            const float transportRight = static_cast<float>(width) - transportLeft;
            const float playX = transportLeft + arrowWidth + gap, nextX = playX + playWidth + gap;
            const float refreshX = transportRight - refreshWidth, repeatX = refreshX - gap - repeatWidth;
            const float valueWidth = std::max(46.0f, std::ceil(ImGui::CalcTextSize("Vol 100").x) + 2.0f);
            const float valueX = nextX + arrowWidth + gap, sliderX = valueX + valueWidth + gap;
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
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                tooltip(std::string(playback.adjacent(1) ? "Forward: " : "Next in library: ") + library.pathLabel(playback.destination(1)));
            ImGui::SetCursorPos({valueX, transportRowY});
            ImGui::Dummy({valueWidth, buttonHeight});
            const auto volumeLabel = "Vol " + std::to_string(static_cast<int>(std::round(audio.volume)));
            centeredLabel(volumeLabel.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
            uiProbe.observe("volume-value", false);
            ImGui::SetCursorPos({sliderX, transportRowY + std::round((buttonHeight - ImGui::GetFrameHeight()) * .5f)});
            ImGui::SetNextItemWidth(repeatX - gap - sliderX);
            // Preserve whole-point mouse/keyboard adjustments with an external readout.
            int volume = static_cast<int>(std::round(audio.volume));
            if (ImGui::SliderInt("##volume", &volume, 0, 100, "")) {
                audio.setVolume(volume);
                preferences.volume = static_cast<int>(std::round(audio.volume));
                ++preferenceRevision;
                nextPreferenceSave = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            }
            uiProbe.observe("volume", ImGui::IsItemClicked());
            rememberControl(); tooltip("Volume: " + std::to_string(static_cast<int>(std::round(audio.volume))) + "%");
            ImGui::SetCursorPos({repeatX, transportRowY});
            if (repeatButton(history.repeat, {repeatWidth, buttonHeight})) { history.cycleRepeat(); historyFile.save(history, true); }
            rememberControl(); tooltip(std::string(repeatTooltip(history.repeat)) + (playback.folder.empty() ? "" : " Library progression is scoped to the selected folder."));
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
            draw->AddRect({0, 0}, {static_cast<float>(width - 1), static_cast<float>(height - 1)}, IM_COL32(90, 99, 99, 255));
            }
            ImGui::End();
            sampleWorkspace.draw(audio,[&](const char* name,bool activated){uiProbe.observe(name,activated);});
            if (sampleWorkspace.active()) nextControls.push_back({0,0,static_cast<float>(width),static_cast<float>(height)});
            controls.swap(nextControls); ImGui::Render();
            presentation.paint(frameStart,Presentation::Clock::now(),inputTimestamp,[&] {
                ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
            });
            inputTimestamp = 0;
            schedule.presented(now);
            schedule.caret = UiSchedule::Time::max();
            if (io.WantTextInput && io.ConfigInputTextCursorBlink) {
                if (const auto input = ImGui::GetInputTextState(ImGui::GetActiveID())) {
                    const auto phase = std::fmod(std::max(0.0f, input->CursorAnim), 1.2f);
                    const auto delay = input->CursorAnim < 0 ? .8f - input->CursorAnim : phase <= .8f ? .801f - phase : 1.201f - phase;
                    schedule.caret = now + std::chrono::milliseconds(static_cast<int>(std::ceil(delay * 1000)));
                }
            }
            {
                auto snapshot = preferences.session;
                snapshot.scrollX = savedScrollX; snapshot.scrollY = savedScrollY;
                snapshot.expanded = expanded; snapshot.history = historyView;
                snapshot.expandedHeight = expandedHeight; snapshot.normalHeight = normalHeight; snapshot.normalMaximized = normalMaximized;
                snapshot.search = search; snapshot.view = static_cast<int>(browser.view); snapshot.order = library.order;
                snapshot.availability = browser.availability; snapshot.extension = browser.extension;
                snapshot.minimum = browser.minimum; snapshot.maximum = browser.maximum; snapshot.measuredOnly = browser.measuredOnly;
                snapshot.includedDirectories.clear();
                for (const auto& rule : browser.directories.included) snapshot.includedDirectories.push_back(rule.path);
                snapshot.closedGroups = grouped.closed;
                snapshot.directoryLocation = fs::path(directoryUi.location); snapshot.directorySearch = directoryUi.search;
                const auto windowFlags = SDL_GetWindowFlags(window);
                if (!(windowFlags & SDL_WINDOW_MINIMIZED)) {
                    snapshot.maximized = (windowFlags & SDL_WINDOW_MAXIMIZED) != 0;
                    if (!snapshot.maximized) {
                        SDL_GetWindowPosition(window,&snapshot.x,&snapshot.y);
                        SDL_GetWindowSize(window,&snapshot.width,&snapshot.height); snapshot.positioned = true;
                    }
                }
                if (snapshot != preferences.session) {
                    preferences.session = std::move(snapshot); ++preferenceRevision;
                    nextPreferenceSave = now + std::chrono::milliseconds(300); schedule.request(2);
                }
            }
            if (windowTest) {
                uiProbe.order = library.order;
                size_t queued = 0; for (const auto& entry : history.entries) if (entry.queued) ++queued;
                std::ostringstream features;
                features << "FEATURES " << preferences.topmost << ' ' << preferences.strip << ' ' << preferences.normalize << ' '
                    << audio.gain << ' ' << audio.position << ' ' << audio.duration << ' ' << (waveform ? waveform->frames : 0) << ' '
                    << queued << ' ' << static_cast<int>(browser.view) << ' ' << !playback.folder.empty() << ' ' << audio.meterPeak << ' '
                    << ImGui::GetCurrentContext()->ErrorCountCurrentFrame;
                uiProbe.features = features.str();
                uiProbe.telemetry = "LOOP " + std::to_string(schedule.presents) + " " + std::to_string(schedule.wakeups) + " " + std::to_string(SDL_TextInputActive(window)) + " " + std::to_string(composing);
                uiProbe.telemetry += "\nPANEL " + std::to_string(preferences.transportHeight) + " " + std::to_string(preferences.strip ? 0 : TransportLayout::height(preferences.transportHeight,height,expanded));
                uiProbe.telemetry += "\nDIRS " + std::to_string(browser.directories.included.size()) + " " + std::to_string(browser.directories.excluded.size()) + " " + std::to_string(preferences.pinnedDirectories.size());
                std::ostringstream sessionProbe;
                sessionProbe << "\nSESSION " << expanded << ' ' << browser.availability << ' ' << browser.minimum << ' ' << browser.maximum << ' ' << browser.measuredOnly << ' ' << std::quoted(browser.extension) << ' ' << std::quoted(std::string(search));
                uiProbe.telemetry += sessionProbe.str();
                uiProbe.telemetry += "\nWAVE " + std::to_string(waveformImage.rebuilds) + " " + std::to_string(waveformImage.vertices) + " " + std::to_string(waveformImage.markerRatio) + " " + std::to_string(audio.seekSequence);
                const auto textStats = textPainter.cacheStats();
                const auto& timing = presentation.timing;
                uiProbe.telemetry += "\nRENDERER " + std::string(presentation.name());
                uiProbe.telemetry += "\nFRAME " + std::to_string(timing.build) + " " + std::to_string(timing.draw) + " " + std::to_string(timing.present) + " " + std::to_string(timing.total) + " " + std::to_string(timing.input);
                uiProbe.telemetry += "\nTEXT " + std::to_string(textStats.shaped) + " " + std::to_string(textStats.rasterized) + " " + std::to_string(textStats.bytes);
                uiProbe.telemetry += "\nPLAYBACK_ERROR " + (audio.error.empty() ? playback.notice : audio.error);
                uiProbe.telemetry += "\nSAMPLER " + sampleWorkspace.diagnostics();
                uiProbe.telemetry += "\nPLAYER_SELECTION " + sampling::Json{{"active",waveformSelection.active},{"start",waveformSelection.start},{"end",waveformSelection.end}}.dump();
                std::ostringstream status;
                status << "READY " << width << ' ' << height << ' ' << static_cast<int>(history.repeat) << ' '
                    << (history.current() ? history.current()->id : 0) << ' ' << history.playedCount << ' '
                    << playback.pending.has_value() << ' ' << audio.paused << ' ' << historyView << ' ' << visible.size();
                uiProbe.flush(stateDirectory, status.str());
            }
        }
        playback.finish();
        if (preferenceWriter.collect(true) && preferenceWriter.error.empty()) savedPreferences = preferenceWriter.revision;
        if (preferenceRevision != savedPreferences)
            stateio::write(stateDirectory / "player-ui.txt", preferences.serialize());
        if (historyFile.writable && history.revision != historyFile.saved && !historyFile.notice.empty())
            MessageBoxA(hwnd, historyFile.notice.c_str(), "Prappy Player: history was not saved", MB_OK | MB_ICONWARNING);
    } catch (const std::exception& e) {
        if (uiProbe.enabled) {
            if (args.size() == 2) { std::ofstream error(fs::path(args[1]) / "ui-error.txt"); error << e.what(); }
        }
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR); exitCode = 1;
    }
    // Playback and state writers have shut down. Native text-input/window
    // cleanup must not hold the single-instance lock if Windows stalls.
    singleton.reset();
    if (window) SDL_StopTextInput(window);
    if (graphics) ImGui_ImplSDLRenderer3_Shutdown();
    if (platform) ImGui_ImplSDL3_Shutdown();
    if (imgui) { ImPlot::DestroyContext(); ImGui::DestroyContext(); }
    presentation.close();
    if (uiProbe.enabled) {
        if (const auto gate = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),"PRAPPY_TEST_EXIT_GATE")) {
            const fs::path directory(gate);
            std::ofstream(directory / ("cleanup-"+std::to_string(GetCurrentProcessId())+".ready")) << "ready\n";
            const auto until = SDL_GetTicks()+10000;
            while (fs::exists(directory / "hold") && SDL_GetTicks() < until) SDL_Delay(10);
        }
    }
    if (window) SDL_DestroyWindow(window);
    return exitCode;
}

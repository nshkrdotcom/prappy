// UI setup follows Dear ImGui's MIT-licensed example_sdl3_sdlrenderer3.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <mpv/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr int Width = 500;
constexpr float Scale = Width / 350.0f;
constexpr float u(float value) { return value * Scale; }
constexpr int MinHeight = 276, TransportHeight = 228;
static WNDPROC originalProc;

static std::string utf8(const fs::path& p) {
    auto s = p.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
static std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \r\n\t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
static std::wstring key(const fs::path& p) {
    auto s = p.lexically_normal().wstring();
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size())); return s;
}
static fs::path executableDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) throw std::runtime_error("Cannot locate executable directory.");
    buf.resize(n); return fs::path(buf).parent_path();
}
struct Config { std::vector<fs::path> roots; std::set<std::string> extensions; };
static Config readConfig(const fs::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot read player.ini beside the executable.");
    Config config;
    std::string line; int number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Config line " + std::to_string(number) + ": expected key=value.");
        const auto name = lower(trim(line.substr(0, equal)));
        auto value = trim(line.substr(equal + 1));
        if (name == "directory") {
            if (value.empty()) throw std::runtime_error("Config: directory cannot be empty.");
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            fs::path path = std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size());
            if (path.is_relative()) path = file.parent_path() / path;
            config.roots.push_back(fs::absolute(path).lexically_normal());
        } else if (name == "extensions") {
            std::istringstream values(value); std::string ext;
            while (std::getline(values, ext, ',')) {
                ext = lower(trim(ext)); if (ext.starts_with('.')) ext.erase(0, 1);
                if (ext.empty() || ext.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos)
                    throw std::runtime_error("Config: invalid extension.");
                config.extensions.insert('.' + ext);
            }
        } else throw std::runtime_error("Config line " + std::to_string(number) + ": unknown key " + name);
    }
    if (config.roots.empty() || config.extensions.empty()) throw std::runtime_error("Config needs directory and extensions entries.");
    return config;
}
static void createDefaultConfig(const fs::path& file) {
    if (fs::exists(file)) return;
    PWSTR music = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music))) throw std::runtime_error("Cannot locate your Music folder.");
    const auto root = utf8(fs::path(music)); CoTaskMemFree(music);
    std::ofstream out(file);
    out << "# UTF-8. Repeat directory= for more roots. Relative roots resolve beside this file.\n"
        << "directory=" << root << "\nextensions=flac,wav,mp3,opus,m4a,wv\n";
    if (!out) throw std::runtime_error("Cannot create player.ini. Put the player in a writable folder.");
}
struct Track { fs::path path; std::string name, directory, type; std::wstring id; };
struct Library {
    std::vector<Track> tracks;
    int order = 0;
    std::string notice;
    void sort() {
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            if (order == 1 && a.directory != b.directory) return a.directory < b.directory;
            if (order == 2 && a.type != b.type) return a.type < b.type;
            auto an = lower(a.name), bn = lower(b.name);
            return an == bn ? a.id < b.id : an < bn;
        });
    }
    bool refresh(const fs::path& configFile) {
        try {
            const auto config = readConfig(configFile);
            std::vector<Track> found; std::set<std::wstring> seen;
            std::vector<std::string> warnings;
            for (const auto& root : config.roots) {
                std::error_code ec;
                if (!fs::is_directory(root, ec)) { warnings.push_back("Missing root: " + utf8(root)); continue; }
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                if (ec) { warnings.push_back("Cannot scan: " + utf8(root)); continue; }
                while (it != end) {
                    const auto entry = *it;
                    const auto attr = GetFileAttributesW(entry.path().c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) && entry.is_directory(ec))
                        it.disable_recursion_pending();
                    if (entry.is_regular_file(ec)) {
                        auto ext = lower(utf8(entry.path().extension()));
                        if (config.extensions.contains(ext)) {
                            auto path = fs::weakly_canonical(entry.path(), ec);
                            if (ec) { path = entry.path().lexically_normal(); ec.clear(); }
                            auto id = key(path);
                            if (seen.insert(id).second) found.push_back({path, utf8(path.filename()), utf8(path.parent_path()), ext.substr(1), id});
                        }
                    }
                    ec.clear(); it.increment(ec);
                    if (ec) { warnings.push_back("Some files could not be read: " + utf8(root)); break; }
                }
            }
            tracks = std::move(found); sort();
            notice = std::to_string(tracks.size()) + " tracks";
            if (!warnings.empty()) notice += " | " + warnings.front() + (warnings.size() > 1 ? " (+more)" : "");
            else if (tracks.empty()) notice += " | Edit player.ini, then Refresh";
            return true;
        } catch (const std::exception& e) { notice = e.what(); return false; }
    }
};

// libmpv exposes a C ABI. Resolve it directly: no MinGW C++/import-library dependency.
struct Audio {
    HMODULE dll = nullptr;
    mpv_handle* handle = nullptr;
#define MPV_API(name) decltype(&mpv_##name) name = nullptr
    MPV_API(create); MPV_API(initialize); MPV_API(terminate_destroy); MPV_API(set_option_string);
    MPV_API(command); MPV_API(set_property); MPV_API(get_property); MPV_API(wait_event); MPV_API(error_string); MPV_API(free);
#undef MPV_API
    std::string error;
    fs::path current;
    bool loaded = false, paused = false, active = false, waitingStart = false;
    double position = 0, duration = 0, volume = 70;
    ~Audio() { if (handle) terminate_destroy(handle); if (dll) FreeLibrary(dll); }
    void open(bool silent = false) {
        auto path = executableDir() / "libmpv-2.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw std::runtime_error("Cannot load libmpv-2.dll (Windows error " + std::to_string(GetLastError()) + ").");
#define MPV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, "mpv_" #name)); if (!name) throw std::runtime_error("Incompatible libmpv: " #name)
        MPV_LOAD(create); MPV_LOAD(initialize); MPV_LOAD(terminate_destroy); MPV_LOAD(set_option_string);
        MPV_LOAD(command); MPV_LOAD(set_property); MPV_LOAD(get_property); MPV_LOAD(wait_event); MPV_LOAD(error_string); MPV_LOAD(free);
#undef MPV_LOAD
        handle = create(); if (!handle) throw std::runtime_error("libmpv could not create the player.");
        for (auto [name, value] : std::array<std::pair<const char*, const char*>, 8>{{
            {"config", "no"}, {"video", "no"}, {"audio-display", "no"}, {"terminal", "no"},
            {"input-default-bindings", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"idle", "yes"}}})
            check(set_option_string(handle, name, value));
        if (silent) check(set_option_string(handle, "ao", "null"));
        check(initialize(handle)); check(set_property(handle, "volume", MPV_FORMAT_DOUBLE, &volume));
    }
    void check(int result) { if (result < 0) throw std::runtime_error(error_string(result)); }
    bool run(std::initializer_list<const char*> args) {
        std::vector<const char*> list(args); list.push_back(nullptr);
        int result = command(handle, list.data());
        if (result < 0) { error = error_string(result); return false; } return true;
    }
    bool play(const fs::path& path) {
        poll(); // Consume the previous track's events before replacing its identity.
        const auto file = utf8(path);
        if (!run({"loadfile", file.c_str(), "replace"})) return false;
        current = path; loaded = false; active = true; waitingStart = true; position = duration = 0; paused = false; error.clear();
        int no = 0; set_property(handle, "pause", MPV_FORMAT_FLAG, &no); return true;
    }
    void pause() {
        if (!active) return;
        int value = paused ? 0 : 1;
        int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &value);
        if (result < 0) error = error_string(result); else paused = value != 0;
    }
    void seek(double seconds) {
        if (!loaded) return;
        const auto value = std::to_string(seconds); run({"seek", value.c_str(), "absolute+exact"});
    }
    void setVolume(double value) {
        int result = set_property(handle, "volume", MPV_FORMAT_DOUBLE, &value);
        if (result < 0) error = error_string(result); else volume = value;
    }
    bool poll() {
        bool eof = false;
        for (;;) {
            auto event = wait_event(handle, 0);
            if (event->event_id == MPV_EVENT_NONE) break;
            if (event->event_id == MPV_EVENT_START_FILE) { waitingStart = false; error.clear(); }
            if (event->event_id == MPV_EVENT_FILE_LOADED) { loaded = true; active = true; }
            if (event->event_id == MPV_EVENT_END_FILE) {
                if (waitingStart) continue;
                auto end = static_cast<mpv_event_end_file*>(event->data);
                if (end->reason == MPV_END_FILE_REASON_ERROR) error = "Playback: " + std::string(error_string(end->error));
                if (end->reason == MPV_END_FILE_REASON_EOF) { eof = true; active = false; loaded = false; }
                if (end->reason == MPV_END_FILE_REASON_ERROR) { active = false; loaded = false; }
            }
        }
        if (loaded) {
            get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
            get_property(handle, "duration", MPV_FORMAT_DOUBLE, &duration);
        }
        return eof;
    }
};
static bool advance(Audio& audio, const Library& library, int direction, bool automatic = false) {
    if (library.tracks.empty()) return false;
    const auto id = key(audio.current);
    auto it = std::find_if(library.tracks.begin(), library.tracks.end(), [&](auto& t) { return t.id == id; });
    auto index = it == library.tracks.end() ? (direction > 0 ? -1LL : 0LL) : static_cast<long long>(it - library.tracks.begin());
    index += direction;
    const auto count = static_cast<long long>(library.tracks.size());
    if (automatic && index >= count) return false;
    index = (index + count) % count;
    return audio.play(library.tracks[static_cast<size_t>(index)].path);
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_GETMINMAXINFO) {
        auto info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {Width, MinHeight}; info->ptMaxTrackSize.x = Width;
        info->ptMaxSize.x = Width; return 0;
    }
    if (message == WM_WINDOWPOSCHANGING) {
        auto pos = reinterpret_cast<WINDOWPOS*>(l);
        if (!(pos->flags & SWP_NOSIZE)) pos->cx = Width;
    }
    if (message == WM_DPICHANGED) {
        auto rect = reinterpret_cast<RECT*>(l);
        RECT old{}; GetWindowRect(hwnd, &old);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, Width, old.bottom - old.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == WM_SYSCOMMAND && (w & 0xfff0) == SC_MAXIMIZE) return 0;
    return CallWindowProcW(originalProc, hwnd, message, w, l);
}
static SDL_HitTestResult SDLCALL hitTest(SDL_Window* window, const SDL_Point* point, void*) {
    int w, h; SDL_GetWindowSize(window, &w, &h);
    if (point->y < 3) return SDL_HITTEST_RESIZE_TOP;
    if (point->y >= h - 5) return SDL_HITTEST_RESIZE_BOTTOM;
    if ((point->y >= 5 && point->y < u(26) && point->x < u(138)) ||
        (point->y >= h - TransportHeight + 3 && point->y < h - TransportHeight + u(19) && point->x < u(100)))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}
static void tooltip(const std::string& text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text.c_str());
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
    s.Colors[ImGuiCol_ButtonHovered] = {.34f, .39f, .40f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {.25f, .48f, .44f, 1};
    s.Colors[ImGuiCol_SliderGrab] = {.47f, .76f, .68f, 1};
    s.Colors[ImGuiCol_Header] = {.25f, .38f, .36f, 1};
    s.ScaleAllSizes(Scale); s.FontSizeBase = 18;
}
static int selfTest(const fs::path& config, const fs::path& logFile) {
    std::ofstream log(logFile);
    try {
        Library library;
        if (!library.refresh(config) || library.tracks.empty()) throw std::runtime_error(library.notice);
        if (library.tracks.size() < 2) throw std::runtime_error("Self-test needs at least two tracks.");
        log << "Scan: " << library.notice << '\n';
        Audio audio; audio.open(true);
        std::set<std::string> formats;
        const auto testTracks = library.tracks;
        for (auto track : testTracks) {
            if (!formats.insert(track.type).second) continue;
            log << "Testing " << track.name << std::endl;
            if (!audio.play(track.path)) throw std::runtime_error(audio.error);
            auto waitLoaded = [&] {
                for (int i = 0; i < 500; ++i) { audio.poll(); if (audio.loaded) return; if (!audio.error.empty()) throw std::runtime_error(audio.error); SDL_Delay(10); }
                throw std::runtime_error("Playback load timeout: " + track.name);
            };
            waitLoaded(); SDL_Delay(120); audio.poll();
            if (audio.duration <= 0) throw std::runtime_error("No duration: " + track.name);
            audio.pause(); audio.poll();
            int paused = 0; audio.get_property(audio.handle, "pause", MPV_FORMAT_FLAG, &paused);
            if (!paused) throw std::runtime_error("Pause failed.");
            double seek = std::min(1.0, audio.duration / 2); audio.seek(seek);
            for (int i = 0; i < 100; ++i) {
                SDL_Delay(10); audio.poll();
                if (std::abs(audio.position - seek) <= .25) break;
            }
            // mpv reports audible position, including the paused audio output buffer.
            if (std::abs(audio.position - seek) > .25) throw std::runtime_error("Seek failed: wanted " + std::to_string(seek) + ", got " + std::to_string(audio.position) + ": " + audio.error);
            audio.setVolume(37); double volume = 0; audio.get_property(audio.handle, "volume", MPV_FORMAT_DOUBLE, &volume);
            if (std::abs(volume - 37) > .1) throw std::runtime_error("Volume failed.");
            auto id = key(audio.current);
            for (int order = 0; order < 3; ++order) { library.order = order; library.sort(); if (key(audio.current) != id) throw std::runtime_error("Sort changed current track."); }
            if (!library.refresh(config) || key(audio.current) != id) throw std::runtime_error("Refresh changed current track.");
            audio.pause(); log << "PASS " << track.type << ": decode / duration / pause / seek / volume / sort / refresh\n";
        }
        if (!advance(audio, library, 1)) throw std::runtime_error("Next failed.");
        SDL_Delay(100); audio.poll();
        auto next = key(audio.current);
        if (!advance(audio, library, -1) || key(audio.current) == next) throw std::runtime_error("Previous failed.");
        log << "PASS next / previous\n";
        library.order = 0; library.sort();
        audio.play(library.tracks.front().path);
        for (int i = 0; i < 500 && !audio.loaded; ++i) { audio.poll(); SDL_Delay(10); }
        audio.seek(std::max(0.0, audio.duration - .15));
        bool eof = false;
        for (int i = 0; i < 500 && !eof; ++i) { eof = audio.poll(); SDL_Delay(10); }
        if (!eof || !advance(audio, library, 1, true) || key(audio.current) != library.tracks[1].id)
            throw std::runtime_error("Automatic progression failed.");
        log << "PASS end-of-track progression\n";
        audio.play(config.parent_path() / L"missing-audio.flac");
        for (int i = 0; i < 300 && audio.error.empty(); ++i) { audio.poll(); SDL_Delay(10); }
        if (audio.error.empty()) throw std::runtime_error("Playback error was not surfaced.");
        log << "PASS visible playback error state: " << audio.error << '\n';
        // Check a real Windows output without producing a test tone in the room.
        Audio output; output.open(); output.setVolume(0); output.play(testTracks.front().path);
        std::string outputDriver;
        for (int i = 0; i < 300; ++i) {
            output.poll();
            char* driver = nullptr;
            if (output.get_property(output.handle, "current-ao", MPV_FORMAT_STRING, &driver) >= 0 && driver) {
                outputDriver = driver; output.free(driver);
            }
            if (!outputDriver.empty()) break;
            if (!output.error.empty()) throw std::runtime_error(output.error);
            SDL_Delay(10);
        }
        if (outputDriver.empty() || outputDriver == "null") throw std::runtime_error("Windows audio output did not initialize.");
        log << "PASS Windows audio output initialized (muted): " << outputDriver << '\n';
        const auto id = key(audio.current);
        const auto count = library.tracks.size();
        if (library.refresh(logFile.parent_path() / "nonexistent-player-config.ini") || library.tracks.size() != count || key(audio.current) != id)
            throw std::runtime_error("Bad config did not preserve playback/library.");
        log << "PASS missing config preserves library and playback\nALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args; for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]); LocalFree(argv);
    if (args.size() == 3 && args[0] == L"--self-test") return selfTest(args[1], args[2]);
    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
    bool imgui = false, platform = false, graphics = false;
    int exitCode = 0;
    try {
        const auto config = executableDir() / "player.ini";
        Library library; Audio audio;
        try { createDefaultConfig(config); } catch (const std::exception& e) { library.notice = e.what(); }
        library.refresh(config);
        audio.open();
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Prappy Player", Width, MinHeight,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(window, Width, MinHeight);
        SDL_SetWindowMaximumSize(window, Width, 8192);
        auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        auto icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SDL_SetWindowHitTest(window, hitTest, nullptr);
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_SetRenderVSync(renderer, 1);
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
        bool done = false, expanded = false, onTop = false;
        int expandedHeight = 710;
        while (!done) {
            const auto start = SDL_GetTicks();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
            }
            if (audio.poll()) advance(audio, library, 1, true);
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(80); continue; }
            ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
            int width, height; SDL_GetWindowSize(window, &width, &height);
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
            ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored({.59f, .83f, .75f, 1}, "PRAPPY  /  PLAYER");
            ImGui::SameLine(u(141));
            if (ImGui::SmallButton(expanded ? "List ^" : "List v")) {
                if (expanded) { expandedHeight = height; SDL_SetWindowSize(window, Width, MinHeight); }
                else SDL_SetWindowSize(window, Width, expandedHeight);
                expanded = !expanded;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(onTop ? "Pin *" : "Pin")) { onTop = !onTop; SDL_SetWindowAlwaysOnTop(window, onTop); }
            tooltip("Keep above other windows"); ImGui::SameLine();
            if (ImGui::SmallButton("_")) SDL_MinimizeWindow(window);
            tooltip("Minimize"); ImGui::SameLine();
            if (ImGui::SmallButton("X")) done = true;
            tooltip("Close");
            const float transportY = static_cast<float>(height - TransportHeight);
            if (expanded && transportY > u(58)) {
                ImGui::SetNextItemWidth(u(153));
                const char* choices[] = {"Filename", "Directory groups", "File type groups"};
                if (ImGui::Combo("##order", &library.order, choices, 3)) library.sort();
                ImGui::SameLine();
                if (ImGui::Button("Config")) {
                    auto result = ShellExecuteW(hwnd, L"open", config.c_str(), nullptr, config.parent_path().c_str(), SW_SHOWNORMAL);
                    if (reinterpret_cast<INT_PTR>(result) <= 32) library.notice = "Could not open player.ini.";
                }
                if (ImGui::BeginChild("Tracks", {0, std::max(1.0f, transportY - ImGui::GetCursorPosY() - u(4))}, ImGuiChildFlags_None)) {
                    const auto current = key(audio.current);
                    auto row = [&](size_t i) {
                        const auto& t = library.tracks[i];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(t.name.c_str(), current == t.id)) audio.play(t.path);
                        tooltip(utf8(t.path)); ImGui::PopID();
                    };
                    if (library.order == 0) {
                        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(library.tracks.size()));
                        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(static_cast<size_t>(i));
                    } else {
                        size_t begin = 0;
                        while (begin < library.tracks.size()) {
                            const auto& first = library.tracks[begin];
                            const auto group = library.order == 1 ? first.directory : first.type;
                            size_t end = begin + 1;
                            while (end < library.tracks.size() && (library.order == 1 ? library.tracks[end].directory : library.tracks[end].type) == group) ++end;
                            ImGui::PushID(static_cast<int>(begin));
                            const bool open = ImGui::CollapsingHeader((group + " (" + std::to_string(end - begin) + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                            tooltip(group);
                            if (open) {
                                ImGuiListClipper clipper; clipper.Begin(static_cast<int>(end - begin));
                                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(begin + static_cast<size_t>(i));
                            }
                            ImGui::PopID(); begin = end;
                        }
                    }
                    if (library.tracks.empty()) ImGui::TextWrapped("Add music roots in Config, then Refresh.");
                }
                ImGui::EndChild();
            }
            // The angled strip is a native drag region. All playback controls stay below it.
            auto draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled({0, transportY}, {static_cast<float>(width), static_cast<float>(height)}, IM_COL32(49, 53, 56, 255));
            const ImVec2 strip[] = {{0, transportY}, {u(96), transportY}, {u(108), transportY + u(16)}, {0, transportY + u(16)}};
            draw->AddConvexPolyFilled(strip, 4, IM_COL32(74, 94, 90, 255));
            for (int x = 14; x < 73; x += 7) draw->AddLine({u(static_cast<float>(x)), transportY + u(6)}, {u(static_cast<float>(x + 3)), transportY + u(10)}, IM_COL32(150, 176, 167, 255));
            ImGui::SetCursorPos({u(8), transportY + u(22)});
            ImGui::TextUnformatted(audio.current.empty() ? "Choose a track from List" : utf8(audio.current.filename()).c_str());
            tooltip(utf8(audio.current));
            ImGui::SetNextItemWidth(u(332));
            float seek = static_cast<float>(audio.position), duration = static_cast<float>(audio.duration);
            ImGui::BeginDisabled(!audio.loaded || duration <= 0);
            if (ImGui::SliderFloat("##seek", &seek, 0, std::max(duration, .01f), "")) audio.seek(seek);
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s / %s", timeText(audio.position).c_str(), timeText(audio.duration).c_str());
            ImGui::SameLine(u(248)); ImGui::TextDisabled("%s", audio.active ? (audio.paused ? "PAUSED" : "PLAYING") : "READY");
            if (ImGui::Button("|<", {u(32), u(24)})) advance(audio, library, -1);
            tooltip("Previous"); ImGui::SameLine();
            if (ImGui::Button(audio.active && !audio.paused ? "Pause" : "Play", {u(56), u(24)})) {
                if (audio.active) audio.pause();
                else if (!audio.current.empty()) audio.play(audio.current);
                else advance(audio, library, 1);
            }
            ImGui::SameLine(); if (ImGui::Button(">|", {u(32), u(24)})) advance(audio, library, 1);
            tooltip("Next"); ImGui::SameLine();
            ImGui::SetNextItemWidth(u(105)); float volume = static_cast<float>(audio.volume);
            if (ImGui::SliderFloat("##volume", &volume, 0, 100, "Vol %.0f")) audio.setVolume(volume);
            ImGui::SameLine();
            if (ImGui::Button("Refresh", {u(65), u(24)})) library.refresh(config);
            tooltip("Reload config and rebuild the music list");
            ImGui::SetCursorPosY(static_cast<float>(height) - u(24));
            ImGui::PushStyleColor(ImGuiCol_Text, audio.error.empty() ? ImVec4(.68f, .72f, .72f, 1) : ImVec4(1, .63f, .5f, 1));
            const auto& message = audio.error.empty() ? library.notice : audio.error;
            ImGui::TextUnformatted(message.c_str()); tooltip(message); ImGui::PopStyleColor();
            draw->AddRect({0, 0}, {static_cast<float>(width - 1), static_cast<float>(height - 1)}, IM_COL32(90, 99, 99, 255));
            ImGui::End(); ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 46, 49, 51, 255); SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
            const auto elapsed = SDL_GetTicks() - start;
            if (elapsed < 33) SDL_Delay(static_cast<Uint32>(33 - elapsed));
        }
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR); exitCode = 1;
    }
    if (graphics) ImGui_ImplSDLRenderer3_Shutdown();
    if (platform) ImGui_ImplSDL3_Shutdown();
    if (imgui) ImGui::DestroyContext();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit(); return exitCode;
}

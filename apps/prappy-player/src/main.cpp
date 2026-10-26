// UI setup follows Dear ImGui's MIT-licensed example_sdl3_sdlrenderer3.
#include <windows.h>
#include <bcrypt.h>
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
constexpr int Width = 500;
constexpr float Scale = Width / 350.0f;
constexpr float u(float value) { return value * Scale; }
constexpr int MinHeight = 250, TransportHeight = 202;
static WNDPROC originalProc;
static std::vector<SDL_FRect> controls, nextControls;
static void rememberControl() {
    if (!ImGui::IsItemVisible()) return;
    auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const auto window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    min.x = std::max(min.x, window.x); min.y = std::max(min.y, window.y);
    max.x = std::min(max.x, window.x + size.x); max.y = std::min(max.y, window.y + size.y);
    nextControls.push_back({min.x, min.y, max.x - min.x, max.y - min.y});
}

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
static fs::path extendedPath(const fs::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\")) return path;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
static fs::path displayPath(const fs::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
    if (value.starts_with(L"\\\\?\\")) return value.substr(4);
    return path;
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
struct Track {
    fs::path path;
    std::string name, directory, type;
    std::wstring id;
    uintmax_t bytes = 0;
    std::vector<fs::path> copies;
};
struct ScanProgress {
    std::atomic<size_t> found{0}, checked{0}, total{0};
    std::atomic<bool> dedup{false};
};
static bool fingerprint(const Track& track, uint64_t& hash) {
    std::ifstream file(extendedPath(track.path), std::ios::binary);
    if (!file) return false;
    std::array<char, 4096> buffer;
    hash = 14695981039346656037ULL;
    auto sample = [&](size_t count) {
        file.read(buffer.data(), static_cast<std::streamsize>(count));
        if (file.gcount() != static_cast<std::streamsize>(count)) return false;
        for (size_t i = 0; i < count; ++i) { hash ^= static_cast<unsigned char>(buffer[i]); hash *= 1099511628211ULL; }
        return true;
    };
    if (!sample(static_cast<size_t>(std::min<uintmax_t>(buffer.size(), track.bytes)))) return false;
    if (track.bytes > buffer.size()) {
        const auto tail = std::min<uintmax_t>(buffer.size(), track.bytes - buffer.size());
        file.seekg(static_cast<std::streamoff>(track.bytes - tail));
        if (!sample(static_cast<size_t>(tail))) return false;
    }
    return true;
}
using Digest = std::array<unsigned char, 32>;
static std::optional<Digest> digest(const Track& track, std::stop_token stop) {
    std::ifstream file(extendedPath(track.path), std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<unsigned char> buffer(1024 * 1024);
    for (uintmax_t remaining = track.bytes; ok && remaining;) {
        if (stop.stop_requested()) { ok = false; break; }
        const auto count = static_cast<ULONG>(std::min<uintmax_t>(remaining, buffer.size()));
        file.read(reinterpret_cast<char*>(buffer.data()), count);
        ok = file.gcount() == count && BCryptHashData(hash, buffer.data(), count, 0) >= 0;
        remaining -= count;
    }
    Digest result{};
    if (ok) ok = file.peek() == std::char_traits<char>::eof() && !file.bad() &&
                 BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? std::optional<Digest>(result) : std::nullopt;
}
static size_t removeDuplicates(std::vector<Track>& tracks, std::stop_token stop, ScanProgress* progress) {
    // Sample only same-size candidates; samples never establish equality.
    // Confirm full-file SHA-256 before collapsing a copy. No persistent index.
    std::sort(tracks.begin(), tracks.end(), [](auto& a, auto& b) { return a.id < b.id; });
    std::map<uintmax_t, size_t> sizes;
    for (auto& t : tracks) ++sizes[t.bytes];
    struct Bucket { std::optional<size_t> first; std::map<Digest, size_t> hashes; };
    std::map<std::pair<uintmax_t, uint64_t>, Bucket> candidates;
    using FileIdentity = std::tuple<std::wstring, DWORD, DWORD, DWORD, uintmax_t>;
    std::map<FileIdentity, size_t> hardLinks;
    std::vector<Track> unique;
    unique.reserve(tracks.size());
    size_t duplicates = 0;
    if (progress) { progress->total = tracks.size(); progress->dedup = true; }
    for (auto& track : tracks) {
        if (stop.stop_requested()) throw std::runtime_error("Scan cancelled.");
        if (progress) ++progress->checked;
        std::optional<FileIdentity> trackIdentity;
        if (sizes[track.bytes] > 1) {
            HANDLE handle = CreateFileW(extendedPath(track.path).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION info{};
                const bool linked = GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks > 1 &&
                                    (info.nFileIndexHigh || info.nFileIndexLow);
                CloseHandle(handle);
                if (linked) {
                    auto scope = track.path.root_path().wstring();
                    auto path = track.path.wstring();
                    if (path.starts_with(L"\\\\")) {
                        const auto serverEnd = path.find(L'\\', 2);
                        scope = path.substr(0, path.find(L'\\', serverEnd + 1));
                    }
                    auto identity = std::make_tuple(scope, info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow, track.bytes);
                    auto it = hardLinks.find(identity);
                    if (it != hardLinks.end()) { unique[it->second].copies.push_back(track.path); ++duplicates; continue; }
                    trackIdentity = identity;
                }
            }
        }
        uint64_t hash = 0;
        if (sizes[track.bytes] > 1 && fingerprint(track, hash)) {
            auto& bucket = candidates[{track.bytes, hash}];
            if (!bucket.first && bucket.hashes.empty()) {
                bucket.first = unique.size();
            } else {
                if (bucket.first) {
                    const auto i = *bucket.first;
                    if (auto value = digest(unique[i], stop)) bucket.hashes.emplace(*value, i);
                    bucket.first.reset();
                }
                if (auto value = digest(track, stop)) {
                    auto [it, inserted] = bucket.hashes.emplace(*value, unique.size());
                    if (!inserted) {
                        if (trackIdentity) hardLinks.emplace(*trackIdentity, it->second);
                        unique[it->second].copies.push_back(track.path); ++duplicates; continue;
                    }
                }
            }
        }
        if (trackIdentity) hardLinks.emplace(*trackIdentity, unique.size());
        unique.push_back(std::move(track));
    }
    tracks = std::move(unique);
    return duplicates;
}
struct Library {
    std::vector<Track> tracks;
    int order = 0;
    std::string notice;
    static bool matches(const Track& track, const std::wstring& id) {
        return track.id == id || std::any_of(track.copies.begin(), track.copies.end(), [&](auto& p) { return key(p) == id; });
    }
    void sort() {
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            if (order == 1 && a.directory != b.directory) return a.directory < b.directory;
            if (order == 2 && a.type != b.type) return a.type < b.type;
            auto an = lower(a.name), bn = lower(b.name);
            return an == bn ? a.id < b.id : an < bn;
        });
    }
    bool refresh(const fs::path& configFile, std::stop_token stop = {}, ScanProgress* progress = nullptr) {
        try {
            const auto config = readConfig(configFile);
            std::vector<Track> found; std::set<std::wstring> seen;
            std::vector<std::string> warnings;
            for (const auto& root : config.roots) {
                std::error_code ec;
                if (!fs::is_directory(root, ec)) { warnings.push_back("Missing root: " + utf8(root)); continue; }
                // Canonicalize once per root. Doing this for every audio file creates
                // thousands of redundant UNC/WSL metadata round trips.
                auto scanRoot = fs::weakly_canonical(root, ec);
                if (ec) { scanRoot = root; ec.clear(); }
                fs::recursive_directory_iterator it(extendedPath(scanRoot), fs::directory_options::skip_permission_denied, ec), end;
                if (ec) { warnings.push_back("Cannot scan: " + utf8(root)); continue; }
                while (it != end) {
                    if (stop.stop_requested()) throw std::runtime_error("Scan cancelled.");
                    const auto entry = *it;
                    if (entry.is_directory(ec)) {
                        const auto attr = GetFileAttributesW(entry.path().c_str());
                        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) it.disable_recursion_pending();
                    }
                    if (entry.is_regular_file(ec)) {
                        auto ext = lower(utf8(entry.path().extension()));
                        if (config.extensions.contains(ext)) {
                            auto path = displayPath(entry.path().lexically_normal());
                            auto id = key(path);
                            const auto bytes = entry.file_size(ec);
                            if (!ec && seen.insert(id).second) found.push_back({path, utf8(path.filename()), utf8(path.parent_path()), ext.substr(1), id, bytes, {}});
                            if (progress) progress->found = found.size();
                        }
                    }
                    ec.clear(); it.increment(ec);
                    if (ec) { warnings.push_back("Scan: " + ec.message() + " | " + utf8(root)); break; }
                }
            }
            const auto duplicates = removeDuplicates(found, stop, progress);
            tracks = std::move(found); sort();
            notice = std::to_string(tracks.size()) + " tracks";
            if (duplicates) notice += " | " + std::to_string(duplicates) + " identical copies hidden";
            if (!warnings.empty()) notice += " | " + warnings.front() + (warnings.size() > 1 ? " (+more)" : "");
            else if (tracks.empty()) notice += " | Edit player.ini, then Refresh";
            return true;
        } catch (const std::exception& e) { notice = e.what(); return false; }
    }
};
// One cancellable scan, started only on launch or by the Refresh button.
// Worker owns its result; the UI swaps it in only after completion.
struct Scan {
    ScanProgress progress;
    std::stop_source cancel;
    std::future<std::pair<Library, bool>> job;
    ~Scan() { cancel.request_stop(); if (job.valid()) job.wait(); }
    bool running() const { return job.valid(); }
    void begin(const fs::path& config, int order) {
        if (running()) return;
        progress.found = progress.checked = progress.total = 0; progress.dedup = false;
        cancel = std::stop_source{};
        job = std::async(std::launch::async, [this, config, order, stop = cancel.get_token()] {
            Library next; next.order = order;
            bool success = next.refresh(config, stop, &progress);
            return std::make_pair(std::move(next), success);
        });
    }
    void update(Library& library) {
        if (!running() || job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto [next, success] = job.get();
        if (success) { next.order = library.order; next.sort(); library = std::move(next); }
        else library.notice = std::move(next.notice);
    }
    std::string status() const {
        if (progress.dedup) return "Checking duplicates: " + std::to_string(progress.checked.load()) + " / " + std::to_string(progress.total.load());
        return "Scanning music: " + std::to_string(progress.found.load()) + " files";
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
    auto it = std::find_if(library.tracks.begin(), library.tracks.end(), [&](auto& t) { return Library::matches(t, id); });
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
    if (ImGui::GetCurrentContext() && (ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))) return SDL_HITTEST_NORMAL;
    for (const auto& rect : controls)
        if (point->x >= rect.x && point->x < rect.x + rect.w && point->y >= rect.y && point->y < rect.y + rect.h)
            return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
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
        for (const auto& track : testTracks) {
            if (track.copies.empty()) continue;
            const auto copy = track.copies.front();
            audio.play(copy);
            if (!library.refresh(config)) throw std::runtime_error("Alias refresh failed.");
            const auto id = key(copy);
            auto current = std::find_if(library.tracks.begin(), library.tracks.end(), [&](auto& t) { return Library::matches(t, id); });
            if (current == library.tracks.end()) throw std::runtime_error("Refresh lost alternate-copy identity.");
            auto nextIndex = (static_cast<size_t>(current - library.tracks.begin()) + 1) % library.tracks.size();
            if (!advance(audio, library, 1) || !Library::matches(library.tracks[nextIndex], key(audio.current)))
                throw std::runtime_error("Next from alternate copy failed.");
            log << "PASS alternate-copy identity across refresh and next\n";
            break;
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
    std::unique_ptr<void, decltype(&CloseHandle)> singleton(nullptr, CloseHandle);
    try {
        const bool windowTest = args.size() == 1 && args[0] == L"--window-test";
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
        const auto config = executableDir() / "player.ini";
        Library library; Audio audio;
        Scan scan;
        try { createDefaultConfig(config); } catch (const std::exception& e) { library.notice = e.what(); }
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
        scan.begin(config, library.order);
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
            scan.update(library);
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(80); continue; }
            ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
            nextControls.clear();
            int width, height; SDL_GetWindowSize(window, &width, &height);
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
            ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored({.25f, .94f, .72f, 1}, "PRAPPY  /  PLAYER");
            const char* listLabel = expanded ? "List ^" : "List v";
            const char* pinLabel = onTop ? "Pin *" : "Pin";
            const auto& uiStyle = ImGui::GetStyle();
            float buttonsWidth = uiStyle.ItemSpacing.x * 3 + uiStyle.FramePadding.x * 8;
            for (const char* label : {listLabel, pinLabel, "_", "X"}) buttonsWidth += ImGui::CalcTextSize(label).x;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonsWidth);
            if (ImGui::SmallButton(listLabel)) {
                if (expanded) { expandedHeight = height; SDL_SetWindowSize(window, Width, MinHeight); }
                else SDL_SetWindowSize(window, Width, expandedHeight);
                expanded = !expanded;
            }
            rememberControl();
            ImGui::SameLine();
            if (ImGui::SmallButton(pinLabel)) { onTop = !onTop; SDL_SetWindowAlwaysOnTop(window, onTop); }
            rememberControl();
            tooltip("Keep above other windows"); ImGui::SameLine();
            if (ImGui::SmallButton("_")) SDL_MinimizeWindow(window);
            rememberControl();
            tooltip("Minimize"); ImGui::SameLine();
            if (ImGui::SmallButton("X")) done = true;
            rememberControl();
            tooltip("Close");
            const float transportY = static_cast<float>(height - TransportHeight);
            if (expanded && transportY > u(58)) {
                ImGui::SetNextItemWidth(u(153));
                const char* choices[] = {"Filename", "Directory groups", "File type groups"};
                if (ImGui::Combo("##order", &library.order, choices, 3)) library.sort();
                rememberControl();
                ImGui::SameLine();
                if (ImGui::Button("Config")) {
                    auto result = ShellExecuteW(hwnd, L"open", config.c_str(), nullptr, config.parent_path().c_str(), SW_SHOWNORMAL);
                    if (reinterpret_cast<INT_PTR>(result) <= 32) library.notice = "Could not open player.ini.";
                }
                rememberControl();
                if (ImGui::BeginChild("Tracks", {0, std::max(1.0f, transportY - ImGui::GetCursorPosY() - u(4))}, ImGuiChildFlags_None)) {
                    const auto current = key(audio.current);
                    auto row = [&](size_t i) {
                        const auto& t = library.tracks[i];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(t.name.c_str(), Library::matches(t, current))) audio.play(t.path);
                        rememberControl();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            std::string paths = utf8(t.path);
                            if (!t.copies.empty()) {
                                paths += "\n\nIdentical copies (" + std::to_string(t.copies.size()) + "):";
                                for (auto& copy : t.copies) paths += "\n" + utf8(copy);
                            }
                            ImGui::SetTooltip("%s", paths.c_str());
                        }
                        ImGui::PopID();
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
                            rememberControl();
                            tooltip(group);
                            if (open) {
                                ImGuiListClipper clipper; clipper.Begin(static_cast<int>(end - begin));
                                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(begin + static_cast<size_t>(i));
                            }
                            ImGui::PopID(); begin = end;
                        }
                    }
                    if (library.tracks.empty()) ImGui::TextWrapped("Add music roots in Config, then Refresh.");
                    if (ImGui::GetScrollMaxY() > 0) {
                        auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                        const auto bar = ImGui::GetStyle().ScrollbarSize;
                        nextControls.push_back({pos.x + size.x - bar, pos.y, bar, size.y});
                    }
                }
                ImGui::EndChild();
            }
            auto draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled({0, transportY}, {static_cast<float>(width), static_cast<float>(height)}, IM_COL32(49, 53, 56, 255));
            ImGui::SetCursorPos({u(8), transportY + u(4)});
            ImGui::TextUnformatted(audio.current.empty() ? "Choose a track from List" : utf8(audio.current.filename()).c_str());
            tooltip(utf8(audio.current));
            ImGui::SetNextItemWidth(u(332));
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
            if (ImGui::Button("|<", {u(32), u(24)})) advance(audio, library, -1);
            rememberControl();
            tooltip("Previous"); ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {.96f, .74f, .39f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, .84f, .55f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.84f, .59f, .24f, 1});
            ImGui::PushStyleColor(ImGuiCol_Text, {.12f, .15f, .16f, 1});
            if (ImGui::Button(audio.active && !audio.paused ? "Pause" : "Play", {u(56), u(24)})) {
                if (audio.active) audio.pause();
                else if (!audio.current.empty()) audio.play(audio.current);
                else advance(audio, library, 1);
            }
            rememberControl();
            ImGui::PopStyleColor(4);
            ImGui::SameLine(); if (ImGui::Button(">|", {u(32), u(24)})) advance(audio, library, 1);
            rememberControl();
            tooltip("Next"); ImGui::SameLine();
            ImGui::SetNextItemWidth(u(105)); float volume = static_cast<float>(audio.volume);
            if (ImGui::SliderFloat("##volume", &volume, 0, 100, "Vol %.0f")) audio.setVolume(volume);
            rememberControl();
            ImGui::SameLine();
            ImGui::BeginDisabled(scan.running());
            if (ImGui::Button(scan.running() ? "Scanning" : "Refresh", {u(65), u(24)})) scan.begin(config, library.order);
            rememberControl();
            ImGui::EndDisabled();
            tooltip("Reload config and rebuild the music list");
            ImGui::SetCursorPosY(static_cast<float>(height) - u(24));
            ImGui::PushStyleColor(ImGuiCol_Text, audio.error.empty() ? ImVec4(.68f, .72f, .72f, 1) : ImVec4(1, .63f, .5f, 1));
            const auto message = !audio.error.empty() ? audio.error : (scan.running() ? scan.status() : library.notice);
            ImGui::TextUnformatted(message.c_str()); tooltip(message); ImGui::PopStyleColor();
            draw->AddRect({0, 0}, {static_cast<float>(width - 1), static_cast<float>(height - 1)}, IM_COL32(90, 99, 99, 255));
            ImGui::End(); controls.swap(nextControls); ImGui::Render();
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

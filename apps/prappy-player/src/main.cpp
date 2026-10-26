// UI setup follows Dear ImGui's MIT-licensed example_sdl3_sdlrenderer3.
#include <windows.h>
#include <bcrypt.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <initguid.h>
#include <propkey.h>
#include <propsys.h>
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
#include <charconv>
#include "history.h"
#ifdef _DEBUG
#include <crtdbg.h>

// Construct before application globals so the report runs after their destruction.
struct DebugLeakReport {
    ~DebugLeakReport() {
        wchar_t path[32768]{};
        const auto length = GetEnvironmentVariableW(L"PRAPPY_LEAK_REPORT", path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) { _CrtDumpMemoryLeaks(); return; }
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { OutputDebugStringW(L"Cannot write PRAPPY_LEAK_REPORT.\n"); return; }
        const auto mode = _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        const auto previous = _CrtSetReportFile(_CRT_WARN, file);
        const bool leaked = _CrtDumpMemoryLeaks() != FALSE;
        const char* result = leaked ? "FAIL CRT leaks\r\n" : "PASS no CRT leaks\r\n";
        DWORD written = 0;
        WriteFile(file, result, static_cast<DWORD>(std::strlen(result)), &written, nullptr);
        _CrtSetReportFile(_CRT_WARN, previous);
        _CrtSetReportMode(_CRT_WARN, mode);
        CloseHandle(file);
    }
};
static DebugLeakReport debugLeakReport;
#endif

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
static std::wstring searchText(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), count);
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    std::replace(value.begin(), value.end(), L'\\', L'/');
    return value;
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
static std::string readablePath(const fs::path& path, const std::vector<fs::path>& roots) {
    if (path.empty()) return {};
    fs::path relative;
    bool afterScraper = false;
    for (const auto& part : path) {
        if (afterScraper) relative /= part;
        else if (key(part) == L"scraper") afterScraper = true;
    }
    if (afterScraper) return utf8(relative);
    for (const auto& root : roots) {
        relative = path.lexically_relative(root);
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != L"..")
            return utf8(root.filename() / relative);
    }
    return utf8(path);
}
static fs::path executableDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) throw std::runtime_error("Cannot locate executable directory.");
    buf.resize(n); return fs::path(buf).parent_path();
}
struct Config { std::vector<fs::path> roots; std::set<std::string> extensions; size_t historyLimit = 10000; };
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
        } else if (name == "history_limit") {
            size_t limit = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (error != std::errc{} || end != value.data() + value.size() || limit == 0 || limit > stateio::MaxRecords)
                throw std::runtime_error("Config: history_limit must be between 1 and 1000000.");
            config.historyLimit = limit;
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
        << "directory=" << root << "\nextensions=flac,wav,mp3,opus,m4a,wv,mka,webm,ogg,aac,aiff,wma\nhistory_limit=10000\n";
    if (!out) throw std::runtime_error("Cannot create player.ini. Put the player in a writable folder.");
}
struct Track {
    fs::path path;
    std::string name, directory, type;
    std::wstring id;
    uintmax_t bytes = 0;
    std::vector<fs::path> copies;
    std::string label;
    int64_t stamp = 0;
    bool offline = false;
    fs::path sourceRoot;
};
struct ScanProgress {
    std::atomic<size_t> found{0}, checked{0}, total{0};
    std::atomic<size_t> hashReads{0}, hashReused{0};
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
struct Verification {
    uintmax_t bytes = 0;
    int64_t stamp = 0;
    std::optional<uint64_t> sample;
    std::optional<Digest> sha;
};
using VerificationCache = std::map<std::wstring, Verification>;
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
static size_t removeDuplicates(std::vector<Track>& tracks, std::stop_token stop, ScanProgress* progress, VerificationCache* cache = nullptr) {
    // Sample only same-size candidates; samples never establish equality.
    // Reuse verification only for unchanged path/size/mtime metadata.
    // Samples alone never establish equality.
    const auto record = [&](const Track& track) -> Verification* {
        if (!cache || track.stamp == 0) return nullptr;
        auto& value = (*cache)[track.id];
        if (value.bytes != track.bytes || value.stamp != track.stamp) value = {track.bytes, track.stamp, {}, {}};
        return &value;
    };
    const auto sampleFor = [&](const Track& track, uint64_t& value) {
        auto saved = record(track);
        if (saved && saved->sample) { value = *saved->sample; if (progress) ++progress->hashReused; return true; }
        if (track.offline) return false;
        if (progress) ++progress->hashReads;
        if (!fingerprint(track, value)) return false;
        if (saved) saved->sample = value;
        return true;
    };
    const auto digestFor = [&](const Track& track) -> std::optional<Digest> {
        auto saved = record(track);
        if (saved && saved->sha) { if (progress) ++progress->hashReused; return saved->sha; }
        if (track.offline) return {};
        if (progress) ++progress->hashReads;
        auto value = digest(track, stop);
        std::error_code ec;
        const auto stamp = fs::last_write_time(extendedPath(track.path), ec);
        if (ec || (track.stamp && stamp.time_since_epoch().count() != track.stamp)) return {};
        if (saved) saved->sha = value;
        return value;
    };
    std::sort(tracks.begin(), tracks.end(), [](auto& a, auto& b) { return a.offline != b.offline ? !a.offline : a.id < b.id; });
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
        if (sizes[track.bytes] > 1 && !track.offline) {
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
        if (sizes[track.bytes] > 1 && sampleFor(track, hash)) {
            auto& bucket = candidates[{track.bytes, hash}];
            if (!bucket.first && bucket.hashes.empty()) {
                bucket.first = unique.size();
            } else {
                if (bucket.first) {
                    const auto i = *bucket.first;
                    if (auto value = digestFor(unique[i])) bucket.hashes.emplace(*value, i);
                    bucket.first.reset();
                }
                if (auto value = digestFor(track)) {
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
    std::vector<Track> files;
    VerificationCache verified;
    std::vector<fs::path> roots;
    std::set<std::string> extensions;
    size_t historyLimit = 10000;
    bool catalogBackup = true;
    bool forceVerification = false;
    int order = 0;
    std::string notice;
    std::string pathLabel(const fs::path& path) const { return readablePath(path, roots); }
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
    std::vector<size_t> filter(const std::string& query) const {
        std::wistringstream input(searchText(query));
        std::vector<std::wstring> terms;
        for (std::wstring term; input >> term;) terms.push_back(std::move(term));
        std::vector<size_t> visible;
        visible.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (terms.empty()) { visible.push_back(i); continue; }
            const auto& track = tracks[i];
            auto text = searchText(track.label);
            for (const auto& copy : track.copies) text += L'\n' + searchText(pathLabel(copy));
            if (std::all_of(terms.begin(), terms.end(), [&](const auto& term) { return text.find(term) != std::wstring::npos; }))
                visible.push_back(i);
        }
        return visible;
    }
    bool refresh(const fs::path& configFile, std::stop_token stop = {}, ScanProgress* progress = nullptr) {
        try {
            const auto config = readConfig(configFile);
            std::vector<Track> found; std::set<std::wstring> seen;
            std::vector<std::string> warnings;
            std::vector<fs::path> unavailable;
            for (const auto& root : config.roots) {
                std::error_code ec;
                if (!fs::is_directory(root, ec)) { warnings.push_back("Offline root: " + utf8(root)); unavailable.push_back(root); continue; }
                // Canonicalize once per root. Doing this for every audio file creates
                // thousands of redundant UNC/WSL metadata round trips.
                auto scanRoot = fs::weakly_canonical(root, ec);
                if (ec) { scanRoot = root; ec.clear(); }
                fs::recursive_directory_iterator it(extendedPath(scanRoot), fs::directory_options::skip_permission_denied, ec), end;
                if (ec) { warnings.push_back("Cannot scan: " + utf8(root)); unavailable.push_back(root); continue; }
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
                            if (!ec && seen.insert(id).second) {
                                const auto modified = entry.last_write_time(ec);
                                found.push_back({path, utf8(path.filename()), readablePath(path.parent_path(), config.roots), ext.substr(1), id, bytes, {}, readablePath(path, config.roots), ec ? 0 : modified.time_since_epoch().count(), false, root});
                            }
                            if (found.size() > stateio::MaxRecords) throw std::runtime_error("Catalog exceeds one million files.");
                            if (progress) progress->found = found.size();
                        }
                    }
                    ec.clear(); it.increment(ec);
                    if (ec) { warnings.push_back("Scan: " + ec.message() + " | " + utf8(root)); unavailable.push_back(root); break; }
                }
            }
            for (const auto& old : files) {
                const bool retain = std::any_of(unavailable.begin(), unavailable.end(), [&](const auto& root) {
                    return key(old.sourceRoot) == key(root);
                });
                if (retain && config.extensions.contains('.' + old.type) && seen.insert(old.id).second) {
                    auto copy = old; copy.offline = true; found.push_back(std::move(copy));
                }
            }
            std::erase_if(verified, [&](const auto& item) { return !seen.contains(item.first); });
            if (found.size() > stateio::MaxRecords) throw std::runtime_error("Catalog exceeds one million files.");
            const bool sameConfig = roots.size() == config.roots.size() && extensions == config.extensions &&
                std::equal(roots.begin(), roots.end(), config.roots.begin(), [](const auto& a, const auto& b) { return key(a) == key(b); });
            std::map<std::wstring, const Track*> priorFiles;
            for (const auto& file : files) priorFiles.emplace(file.id, &file);
            const bool unchanged = !forceVerification && sameConfig && found.size() == files.size() &&
                std::all_of(found.begin(), found.end(), [&](const auto& file) {
                    const auto old = priorFiles.find(file.id);
                    return old != priorFiles.end() && file.stamp != 0 && file.stamp == old->second->stamp &&
                        file.bytes == old->second->bytes && file.offline == old->second->offline;
                });
            size_t duplicates = 0;
            if (unchanged) {
                files = std::move(found); duplicates = files.size() - tracks.size();
                if (progress) progress->hashReused = files.size();
            } else {
                files = found;
                duplicates = removeDuplicates(found, stop, progress, &verified);
                // Preserve known hard-link/copy groups while their entire root is offline.
                // Their files cannot be opened to rediscover identity until the root returns.
                std::map<std::wstring, std::wstring> priorGroups;
                for (const auto& old : tracks) {
                    priorGroups.emplace(old.id, old.id);
                    for (const auto& copy : old.copies) priorGroups.emplace(key(copy), old.id);
                }
                std::map<std::wstring, size_t> offlineGroups;
                std::vector<Track> reconciled;
                for (auto& track : found) {
                    const auto group = priorGroups.find(track.id);
                    if (track.offline && group != priorGroups.end()) {
                        const auto [it, inserted] = offlineGroups.emplace(group->second, reconciled.size());
                        if (!inserted) {
                            auto& original = reconciled[it->second];
                            original.copies.push_back(track.path);
                            original.copies.insert(original.copies.end(), track.copies.begin(), track.copies.end());
                            duplicates += 1 + track.copies.size();
                            continue;
                        }
                    }
                    reconciled.push_back(std::move(track));
                }
                tracks = std::move(reconciled);
            }
            forceVerification = false;
            roots = config.roots; extensions = config.extensions;
            historyLimit = config.historyLimit; sort();
            notice = std::to_string(tracks.size()) + " tracks";
            if (duplicates) notice += " | " + std::to_string(duplicates) + " identical copies hidden";
            if (!warnings.empty()) notice += " | " + warnings.front() + (warnings.size() > 1 ? " (+more)" : "");
            else if (tracks.empty()) notice += " | Edit player.ini, then Refresh";
            return true;
        } catch (const std::exception& e) { notice = e.what(); return false; }
    }
};
#include "catalog.h"
// One cancellable scan, started only on launch or by the Refresh button.
// Worker owns its result; the UI swaps it in only after completion.
struct Scan {
    ScanProgress progress;
    std::stop_source cancel;
    std::future<std::pair<Library, bool>> job;
    ~Scan() { cancel.request_stop(); if (job.valid()) job.wait(); }
    bool running() const { return job.valid(); }
    void begin(const fs::path& config, int order, const Library* previous = nullptr, bool persist = false) {
        if (running()) return;
        progress.found = progress.checked = progress.total = 0; progress.dedup = false;
        progress.hashReads = progress.hashReused = 0;
        cancel = std::stop_source{};
        Library baseline; if (previous) baseline = *previous;
        job = std::async(std::launch::async, [this, config, order, persist, next = std::move(baseline), stop = cancel.get_token()] () mutable {
            next.order = order;
            bool success = next.refresh(config, stop, &progress);
            if (success && persist && !stop.stop_requested()) {
                try { saveCatalog(config.parent_path() / "player-catalog.bin", next); next.catalogBackup = true; }
                catch (const std::exception& e) { next.notice += " | Catalog save failed: " + std::string(e.what()); }
            }
            return std::make_pair(std::move(next), success);
        });
    }
    bool update(Library& library) {
        if (!running() || job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto [next, success] = job.get();
        if (success) { next.order = library.order; next.sort(); library = std::move(next); }
        else library.notice = std::move(next.notice);
        return success;
    }
    std::string status() const {
        if (progress.dedup) return "Checking duplicates: " + std::to_string(progress.checked.load()) + " / " + std::to_string(progress.total.load());
        return "Scanning music: " + std::to_string(progress.found.load()) + " files";
    }
};

// libmpv exposes a C ABI. Resolve it directly: no MinGW C++/import-library dependency.
struct Audio {
    Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    HMODULE dll = nullptr;
    mpv_handle* handle = nullptr;
#define MPV_API(name) decltype(&mpv_##name) name = nullptr
    MPV_API(create); MPV_API(initialize); MPV_API(terminate_destroy); MPV_API(set_option_string);
    MPV_API(command); MPV_API(set_property); MPV_API(get_property); MPV_API(wait_event); MPV_API(error_string); MPV_API(free);
#undef MPV_API
    std::string error;
    fs::path current;
    bool loaded = false, paused = false, active = false, waitingStart = false;
    int64_t desiredEntry = -1, eventEntry = -1;
    uint64_t serial = 0, loadedSerial = 0;
    double position = 0, duration = 0, volume = 70;
    ~Audio() { if (handle) terminate_destroy(handle); if (dll) FreeLibrary(dll); }
    void open(bool silent = false) {
        if (dll) throw std::logic_error("Audio is already open.");
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
    bool play(const fs::path& path, bool startPaused = false, double startPosition = 0) {
        poll(); // Consume the previous track's events before replacing its identity.
        const auto file = utf8(path);
        int pauseValue = startPaused ? 1 : 0;
        const int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &pauseValue);
        if (result < 0) { error = error_string(result); return false; }
        const auto options = std::string(startPaused ? "pause=yes,start=" : "pause=no,start=") + std::to_string(std::max(0.0, startPosition));
        if (!run({"loadfile", file.c_str(), "replace", "-1", options.c_str()})) return false;
        if (get_property(handle, "playlist/0/id", MPV_FORMAT_INT64, &desiredEntry) < 0) {
            error = "Cannot identify requested playback."; return false;
        }
        ++serial;
        current = path; loaded = false; active = true; waitingStart = true;
        position = startPosition; duration = 0; paused = startPaused; error.clear(); return true;
    }
    void setPaused(bool value) {
        if (!active) return;
        int flag = value ? 1 : 0;
        int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &flag);
        if (result < 0) error = error_string(result); else paused = value;
    }
    void pause() { setPaused(!paused); }
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
            if (event->event_id == MPV_EVENT_START_FILE) {
                eventEntry = static_cast<mpv_event_start_file*>(event->data)->playlist_entry_id;
                if (eventEntry == desiredEntry) { waitingStart = false; error.clear(); }
            }
            if (event->event_id == MPV_EVENT_FILE_LOADED && eventEntry == desiredEntry) {
                loaded = true; active = true; loadedSerial = serial;
            }
            if (event->event_id == MPV_EVENT_END_FILE) {
                auto end = static_cast<mpv_event_end_file*>(event->data);
                if (end->playlist_entry_id != desiredEntry) continue;
                if (end->reason == MPV_END_FILE_REASON_ERROR) error = "Playback: " + std::string(error_string(end->error));
                if (end->reason == MPV_END_FILE_REASON_EOF) { eof = true; active = false; loaded = false; }
                if (end->reason == MPV_END_FILE_REASON_ERROR) { active = false; loaded = false; }
            }
        }
        if (loaded) {
            get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
            get_property(handle, "duration", MPV_FORMAT_DOUBLE, &duration);
            int flag = paused ? 1 : 0;
            if (get_property(handle, "pause", MPV_FORMAT_FLAG, &flag) >= 0) paused = flag != 0;
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
#include "playback.h"

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
static std::string statusText(std::string primary, const std::string& progress = {}) {
    if (!progress.empty()) primary += (primary.empty() ? "" : " \xc2\xb7 ") + progress;
    for (size_t offset = 0; (offset = primary.find(" | ", offset)) != std::string::npos; offset += 4)
        primary.replace(offset, 3, " \xc2\xb7 ");
    return primary;
}
#include "state_tests.h"
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
        Library searchLibrary;
        Track lead; lead.label = "library\\Jazz\\Night Bus.flac"; lead.name = "Night Bus.flac";
        lead.directory = "Jazz"; lead.type = "flac"; lead.id = L"night";
        lead.copies = {L"C:/scraper/library/Alternate Take.opus"};
        Track other; other.label = "library\\Jazz\\Morning.flac"; other.name = "Morning.flac";
        other.directory = "Jazz"; other.type = "flac"; other.id = L"morning";
        Track unicode; unicode.label = "library\\Beyoncé\\Live.m4a"; unicode.name = "Live.m4a";
        unicode.directory = "Beyoncé"; unicode.type = "m4a"; unicode.id = L"live";
        searchLibrary.tracks = {lead, other, unicode};
        for (int order = 0; order < 3; ++order) {
            searchLibrary.order = order; searchLibrary.sort();
            for (const char* query : {"NIGHT jazz", "jazz/night", "alternate OPUS"}) {
                const auto matches = searchLibrary.filter(query);
                if (matches.size() != 1 || searchLibrary.tracks[matches[0]].id != L"night")
                    throw std::runtime_error("Search lost track identity or failed word/path/alias matching.");
            }
        }
        if (searchLibrary.filter("BEYONCÉ").size() != 1 || searchLibrary.filter("  ").size() != 3 ||
            !searchLibrary.filter("no-such-track").empty() || searchLibrary.tracks.size() != 3)
            throw std::runtime_error("Search failed Unicode, clear or no-match behavior.");
        log << "PASS search: words / case / Unicode / paths / copies / sorting / clear / no matches\n";
        Library library;
        if (!library.refresh(config) || library.tracks.empty()) throw std::runtime_error(library.notice);
        if (library.tracks.size() < 2) throw std::runtime_error("Self-test needs at least two tracks.");
        log << "Scan: " << library.notice << '\n';
        Audio audio; audio.open(true);
        std::set<std::string> formats;
        const auto testTracks = library.tracks;
        for (auto track : testTracks) {
            if (!formats.insert(track.type).second) continue;
            log << "Testing " << track.label << std::endl;
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
        log << "PASS missing config preserves library and playback\n";
        for (int cycle = 0; cycle < 12; ++cycle) {
            Audio repeated; repeated.open(true);
            if (!repeated.play(testTracks[static_cast<size_t>(cycle) % testTracks.size()].path))
                throw std::runtime_error(repeated.error);
            for (int i = 0; i < 300 && !repeated.loaded; ++i) {
                repeated.poll();
                if (!repeated.error.empty()) throw std::runtime_error(repeated.error);
                SDL_Delay(10);
            }
            if (!repeated.loaded) throw std::runtime_error("Repeated playback load timeout.");
            if (!library.refresh(config)) throw std::runtime_error("Repeated refresh failed.");
            Scan cancelled; cancelled.begin(config, cycle % 3);
            // Destruction requests cancellation and joins even while a scan is active.
        }
        log << "PASS 12 playback create/load/destroy, refresh and scan cancellation cycles\nALL PASS\n";
        return 0;
    } catch (const std::exception& e) { log << "FAIL " << e.what() << '\n'; return 1; }
}

static int runPlayer(HINSTANCE instance) {
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
            if (scan.update(library)) { history.setLimit(library.historyLimit); filterDirty = true; }
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
                            const auto text = searchText(utf8(history.entries[i - 1].path));
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
                            const auto& entry = history.entries[i];
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
            tooltip(library.pathLabel(audio.current));
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
                if (ImGui::GetIO().KeyShift) { library.verified.clear(); library.forceVerification = true; }
                scan.begin(config, library.order, &library, true);
            }
            rememberControl();
            ImGui::EndDisabled();
            tooltip("Refresh the catalog. Hold Shift to reverify duplicate contents.");
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

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    try { return runPlayer(instance); }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR);
        return 1;
    }
}

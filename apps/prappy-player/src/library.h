#pragma once
#include "config.h"
#include "state_io.h"
#include <bcrypt.h>
#include <array>
#include <optional>
#include <set>
#include <stop_token>
#include <tuple>
#include <map>
#include <memory>
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
    std::wstring search;
    std::wstring searchName;
    size_t searchLabelLength = 0;
    std::string sortName;
    std::optional<double> duration;
};
struct ScanProgress {
    std::atomic<size_t> found{0}, checked{0}, total{0};
    std::atomic<size_t> hashReads{0}, hashReused{0};
    std::atomic<bool> dedup{false};
};
inline bool fingerprint(const Track& track, uint64_t& hash) {
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
inline std::optional<Digest> digest(const Track& track, std::stop_token stop) {
    std::ifstream file(extendedPath(track.path), std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE value) { BCryptCloseAlgorithmProvider(value, 0); };
    const std::unique_ptr<void, decltype(closeAlgorithm)> algorithmOwner(algorithm, closeAlgorithm);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    const std::unique_ptr<void, decltype(&BCryptDestroyHash)> hashOwner(hash, BCryptDestroyHash);
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
    return ok ? std::optional<Digest>(result) : std::nullopt;
}
inline size_t removeDuplicates(std::vector<Track>& tracks, std::stop_token stop, ScanProgress* progress, VerificationCache* cache = nullptr) {
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
    static fs::path pathInFolder(const Track& track, const fs::path& folder) {
        const auto id = key(folder);
        if (key(track.path.parent_path()) == id) return track.path;
        const auto copy = std::find_if(track.copies.begin(), track.copies.end(), [&](const auto& path) { return key(path.parent_path()) == id; });
        return copy == track.copies.end() ? fs::path{} : *copy;
    }
    void rebuildSearch() {
        for (auto& track : tracks) {
            track.sortName = lower(track.name);
            track.search = searchText(track.label);
            track.searchLabelLength = track.search.size(); track.searchName = searchText(track.name);
            for (const auto& copy : track.copies) track.search += L'\n' + searchText(pathLabel(copy));
        }
    }
    void sort() {
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            if (order == 3 && a.duration != b.duration) {
                if (!a.duration || !b.duration) return a.duration.has_value();
                return *a.duration < *b.duration;
            }
            if (order == 1 && a.directory != b.directory) return a.directory < b.directory;
            if (order == 2 && a.type != b.type) return a.type < b.type;
            return a.sortName == b.sortName ? a.id < b.id : a.sortName < b.sortName;
        });
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
            historyLimit = config.historyLimit; rebuildSearch(); sort();
            notice = std::to_string(tracks.size()) + " tracks";
            if (duplicates) notice += " | " + std::to_string(duplicates) + " identical copies hidden";
            if (!warnings.empty()) notice += " | " + warnings.front() + (warnings.size() > 1 ? " (+more)" : "");
            else if (tracks.empty()) notice += " | Edit player.ini, then Refresh";
            return true;
        } catch (const std::exception& e) { notice = e.what(); return false; }
    }
};

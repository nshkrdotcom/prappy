#pragma once
// Included after the catalog model. This schema is independent of product builds.
#include <bit>
inline constexpr uint64_t CatalogVersion = 1;
static std::string catalogBytes(const Library& library) {
    stateio::BinaryWriter payload;
    payload.number(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    payload.number(library.roots.size()); for (const auto& root : library.roots) payload.text(utf8(root));
    payload.number(library.extensions.size()); for (const auto& extension : library.extensions) payload.text(extension);
    payload.number(library.files.size());
    std::map<std::wstring, size_t> indices;
    for (size_t i = 0; i < library.files.size(); ++i) {
        const auto& file = library.files[i]; indices.emplace(file.id, i);
        payload.text(utf8(file.path)); payload.text(utf8(file.sourceRoot));
        payload.number(file.bytes); payload.number(std::bit_cast<uint64_t>(file.stamp));
        payload.number(file.offline ? 1 : 0);
        const auto it = library.verified.find(file.id);
        const Verification* saved = it != library.verified.end() && it->second.bytes == file.bytes && it->second.stamp == file.stamp ? &it->second : nullptr;
        const uint64_t flags = saved ? (saved->sample ? 1ULL : 0ULL) | (saved->sha ? 2ULL : 0ULL) : 0ULL;
        payload.number(flags);
        if (flags & 1) payload.number(*saved->sample);
        if (flags & 2) payload.text(std::string(reinterpret_cast<const char*>(saved->sha->data()), saved->sha->size()));
    }
    payload.number(library.tracks.size());
    for (const auto& track : library.tracks) {
        payload.number(indices.at(track.id)); payload.number(track.copies.size());
        for (const auto& copy : track.copies) payload.number(indices.at(key(copy)));
    }
    stateio::BinaryWriter header; header.bytes = "PRAPCAT\n";
    header.number(CatalogVersion); header.number(payload.bytes.size()); header.number(stateio::checksum(payload.bytes));
    header.bytes += payload.bytes;
    if (header.bytes.size() > stateio::MaxFile) throw std::runtime_error("Catalog exceeds file size limit.");
    return header.bytes;
}
static Library parseCatalog(const std::string& bytes, const Config& config) {
    if (bytes.size() < 32 || bytes.size() > stateio::MaxFile || bytes.substr(0, 8) != "PRAPCAT\n") throw std::runtime_error("Invalid catalog header.");
    stateio::BinaryReader header{std::string_view(bytes).substr(8, 24)};
    if (header.number() != CatalogVersion) throw std::runtime_error("Catalog format changed; rebuilding.");
    if (header.number() != bytes.size() - 32) throw std::runtime_error("Invalid catalog payload length.");
    const auto payload = std::string_view(bytes).substr(32);
    if (header.number() != stateio::checksum(payload)) throw std::runtime_error("Catalog checksum failed.");
    stateio::BinaryReader input{payload}; Library library;
    input.number(); // UTC scan time, reserved for diagnostics/future refresh policies.
    const auto rootCount = input.count(10000);
    for (size_t i = 0; i < rootCount; ++i) library.roots.push_back(stateio::pathFromText(input.text()));
    const auto extensionCount = input.count(10000);
    for (size_t i = 0; i < extensionCount; ++i) library.extensions.insert(input.text());
    std::vector<std::wstring> oldRoots, newRoots;
    for (const auto& root : library.roots) oldRoots.push_back(key(root));
    for (const auto& root : config.roots) newRoots.push_back(key(root));
    if (oldRoots != newRoots || library.extensions != config.extensions) throw std::runtime_error("Catalog configuration changed; rebuilding.");
    library.historyLimit = config.historyLimit;
    const auto count = input.count(); std::set<std::wstring> seen;
    for (size_t i = 0; i < count; ++i) {
        const auto path = stateio::pathFromText(input.text()); const auto id = key(path);
        const auto sourceRoot = stateio::pathFromText(input.text());
        if (std::none_of(library.roots.begin(), library.roots.end(), [&](const auto& root) { return key(root) == key(sourceRoot); }))
            throw std::runtime_error("Unknown catalog source root.");
        if (!seen.insert(id).second) throw std::runtime_error("Duplicate catalog path.");
        const auto length = input.number(); const auto stamp = std::bit_cast<int64_t>(input.number());
        const bool offline = input.count(1) != 0;
        const auto extension = lower(utf8(path.extension()));
        if (!config.extensions.contains(extension)) throw std::runtime_error("Unexpected catalog extension.");
        library.files.push_back({path, utf8(path.filename()), readablePath(path.parent_path(), config.roots), extension.substr(1), id, length, {}, readablePath(path, config.roots), stamp, offline, sourceRoot});
        const auto flags = input.count(3);
        Verification saved{length, stamp, {}, {}};
        if (flags & 1) saved.sample = input.number();
        if (flags & 2) {
            const auto value = input.text();
            if (value.size() != 32) throw std::runtime_error("Invalid catalog hash.");
            Digest sha{}; std::memcpy(sha.data(), value.data(), sha.size()); saved.sha = sha;
        }
        if (flags && stamp) library.verified.emplace(id, saved);
    }
    const auto tracks = input.count(count); std::set<size_t> assigned;
    for (size_t i = 0; i < tracks; ++i) {
        const auto index = input.count(count ? count - 1 : 0);
        if (index >= library.files.size() || !assigned.insert(index).second) throw std::runtime_error("Invalid catalog track reference.");
        auto track = library.files[index];
        const auto copies = input.count(count);
        for (size_t j = 0; j < copies; ++j) {
            const auto copy = input.count(count ? count - 1 : 0);
            if (copy >= library.files.size() || !assigned.insert(copy).second) throw std::runtime_error("Invalid catalog copy reference.");
            track.copies.push_back(library.files[copy].path);
        }
        library.tracks.push_back(std::move(track));
    }
    if (assigned.size() != count) throw std::runtime_error("Incomplete catalog tracks.");
    input.finish(); library.sort();
    library.notice = "Cached catalog: " + std::to_string(library.tracks.size()) + " tracks";
    return library;
}
static void saveCatalog(const fs::path& file, const Library& library) { stateio::write(file, catalogBytes(library), library.catalogBackup); }
static bool loadCatalog(const fs::path& file, const Config& config, Library& library) {
    std::error_code ec;
    if (!fs::exists(file, ec) && !fs::exists(file.wstring() + L".bak", ec)) return false;
    std::string failure;
    try { library = parseCatalog(stateio::read(file), config); return true; }
    catch (const std::exception& e) { failure = e.what(); }
    try {
        library = parseCatalog(stateio::read(file.wstring() + L".bak"), config);
        library.catalogBackup = false;
        library.notice += " | Recovered backup"; return true;
    } catch (const std::exception&) { library.notice = failure; return false; }
}

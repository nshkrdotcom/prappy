#pragma once
#include "state_io.h"
#include "paths.h"
#include "paged_vector.h"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

enum class RepeatMode { Off, One, All };
inline const char* repeatTooltip(RepeatMode mode) {
    switch (mode) {
    case RepeatMode::One: return "Repeat: One. Click to repeat all tracks.";
    case RepeatMode::All: return "Repeat: All. Click to turn repeat off.";
    default: return "Repeat: Off. Click to repeat one track.";
    }
}
struct HistoryEntry {
    uint64_t id = 0;
    int64_t time = 0;
    std::filesystem::path path;
    double position = 0;
    std::wstring search;
    bool queued = false;
};
struct History {
    PagedVector<HistoryEntry> entries;
    std::vector<uint64_t> trail;
    size_t cursor = 0, limit = 10000;
    uint64_t nextId = 1, revision = 0;
    uint64_t visitsRevision = 0;
    size_t playedCount = 0;
    RepeatMode repeat = RepeatMode::Off;
    std::map<uint64_t, size_t> index;
    void reindex() {
        index.clear(); playedCount = 0;
        for (size_t i = 0; i < entries.size(); ++i) { index.emplace(entries[i].id, i); if (!entries[i].queued) ++playedCount; }
    }
    const HistoryEntry* find(uint64_t id) const {
        const auto it = index.find(id); return it == index.end() ? nullptr : &entries[it->second];
    }
    const HistoryEntry* current() const {
        const auto entry = cursor < trail.size() ? find(trail[cursor]) : nullptr;
        return entry && !entry->queued ? entry : nullptr;
    }
    static int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    std::optional<size_t> adjacent(int direction) const {
        if (trail.empty() || (direction < 0 && cursor == 0) || (direction > 0 && cursor + 1 >= trail.size())) return {};
        return direction < 0 ? cursor - 1 : cursor + 1;
    }
    void prune() {
        const auto active = current() ? current()->id : 0;
        const auto played = playedCount;
        const auto cap = std::min(limit, stateio::MaxRecords - (entries.size() - playedCount));
        if (played > cap) {
            ++visitsRevision;
            const auto drop = played - cap;
            std::vector<std::pair<int64_t, uint64_t>> oldest;
            for (const auto& entry : entries) if (!entry.queued && entry.id != active) oldest.emplace_back(entry.time, entry.id);
            const auto boundary = oldest.begin() + static_cast<ptrdiff_t>(drop - 1);
            std::nth_element(oldest.begin(), boundary, oldest.end());
            const auto threshold = *boundary;
            std::vector<HistoryEntry> kept;
            for (const auto& entry : entries) {
                if (entry.queued || entry.id == active || std::make_pair(entry.time, entry.id) > threshold) kept.push_back(entry);
            }
            entries = std::move(kept); reindex();
        }
        std::erase_if(trail, [&](uint64_t id) { return !index.contains(id); });
        const auto it = std::find(trail.begin(), trail.end(), active);
        cursor = it == trail.end() ? (trail.empty() ? 0 : trail.size() - 1) : static_cast<size_t>(it - trail.begin());
    }
    void setLimit(size_t value) { if (value != limit) { limit = value; prune(); ++revision; } }
    uint64_t add(const std::filesystem::path& path) {
        if (nextId == UINT64_MAX) throw std::runtime_error("History visit IDs exhausted.");
        std::vector<uint64_t> scheduled;
        const auto first = current() ? cursor + 1 : 0;
        for (size_t i = first; i < trail.size(); ++i) if (find(trail[i])->queued) scheduled.push_back(trail[i]);
        trail.resize(first);
        const auto id = nextId++;
        entries.push_back({id, now(), path, 0, searchText(utf8(path)), false});
        ++playedCount;
        index.emplace(id, entries.size() - 1); trail.push_back(id); cursor = trail.size() - 1;
        trail.insert(trail.end(), scheduled.begin(), scheduled.end());
        if (playedCount > limit || entries.size() > stateio::MaxRecords) prune();
        ++revision; ++visitsRevision; return id;
    }
    uint64_t enqueue(const fs::path& path, bool next) {
        if (nextId == UINT64_MAX || entries.size() >= stateio::MaxRecords) throw std::runtime_error("Queue/history limit reached.");
        const auto queued = entries.size() - playedCount;
        if (queued >= 10000) throw std::runtime_error("Queue is limited to 10000 tracks.");
        const auto id = nextId++;
        entries.push_back({id, 0, path, 0, searchText(utf8(path)), true}); index.emplace(id, entries.size() - 1);
        const auto place = next ? (current() ? cursor + 1 : 0) : trail.size();
        trail.insert(trail.begin() + static_cast<ptrdiff_t>(place), id);
        ++revision; return id;
    }
    void removeQueued(uint64_t id) {
        const auto entry = find(id); if (!entry || !entry->queued) return;
        const auto active = current() ? current()->id : 0;
        std::vector<HistoryEntry> kept;
        for (const auto& item : entries) if (item.id != id) kept.push_back(item);
        entries = std::move(kept); reindex(); std::erase(trail, id);
        const auto found = std::find(trail.begin(), trail.end(), active);
        cursor = found == trail.end() ? 0 : static_cast<size_t>(found - trail.begin()); ++revision;
    }
    void visit(size_t target) {
        if (target >= trail.size()) throw std::runtime_error("Invalid history destination.");
        const bool scheduled = find(trail[target])->queued;
        if (scheduled) {
            auto& entry = entries[index.at(trail[target])]; entry.queued = false; entry.time = now();
            ++visitsRevision; ++playedCount;
        }
        cursor = target; ++revision;
        if (scheduled && playedCount > limit) prune();
    }
    void revisit(uint64_t id) {
        const auto entry=find(id);
        if(!entry||entry->queued)throw std::runtime_error("Invalid listening history visit.");
        auto found=std::find(trail.begin(),trail.end(),id);
        if(found==trail.end())found=trail.insert(trail.begin()+static_cast<ptrdiff_t>(current()?cursor+1:trail.size()),id);
        visit(static_cast<size_t>(found-trail.begin()));
    }
    void position(double value) {
        if (!std::isfinite(value) || value < 0 || !current()) return;
        auto& entry = entries[index.at(current()->id)];
        if (entry.position != value) { entry.position = value; ++revision; }
    }
    void cycleRepeat() { repeat = static_cast<RepeatMode>((static_cast<int>(repeat) + 1) % 3); ++revision; }
    struct Snapshot {
        PagedVector<HistoryEntry> entries;
        std::vector<uint64_t> trail;
        uint64_t nextId, currentId;
        RepeatMode repeat;
        std::string serialize() const {
            std::ostringstream out; out.imbue(std::locale::classic());
            out << "PRAPPY_HISTORY 1\nNEXT_ID " << nextId << "\nREPEAT " << static_cast<int>(repeat) << '\n';
            for (const auto& entry : entries) {
                out << (entry.queued ? "QUEUED " : "ENTRY ") << entry.id << ' ' << entry.time << ' ' << std::setprecision(17) << entry.position << ' '
                    << std::quoted(stateio::pathText(entry.path)) << '\n';
                if (out.tellp() > static_cast<std::streamoff>(stateio::MaxFile)) throw std::runtime_error("History exceeds file size limit.");
            }
            out << "TRAIL"; for (auto id : trail) out << ' ' << id;
            out << "\nCURSOR " << currentId << '\n';
            const auto result = out.str();
            if (result.size() > stateio::MaxFile) throw std::runtime_error("History exceeds the file size limit.");
            return result;
        }
    };
    Snapshot snapshot() const { return {entries, trail, nextId, current() ? current()->id : 0, repeat}; }
    std::string serialize() const {
        return snapshot().serialize();
    }
    static History parse(const std::string& bytes, size_t cap) {
        if (!cap || cap > stateio::MaxRecords) throw std::runtime_error("Invalid history limit.");
        if (bytes.size() > stateio::MaxFile) throw std::runtime_error("History is too large.");
        std::istringstream input(bytes.starts_with("\xef\xbb\xbf") ? bytes.substr(3) : bytes); input.imbue(std::locale::classic());
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error("Empty history file.");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "PRAPPY_HISTORY 1") {
            if (line.starts_with("PRAPPY_HISTORY ")) throw stateio::FutureVersion("Unsupported history version; file preserved.");
            throw std::runtime_error("Invalid history header.");
        }
        History result; result.limit = cap;
        bool nextSeen = false, cursorSeen = false, trailSeen = false, repeatSeen = false;
        uint64_t currentId = 0, previousId = 0;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // TRAIL can contain one million bounded numeric IDs.
            if (line.size() > 24 * stateio::MaxRecords) throw std::runtime_error("History record too large.");
            std::istringstream record(line); record.imbue(std::locale::classic());
            std::string kind; record >> kind;
            if (kind == "ENTRY" || kind == "QUEUED") {
                if (line.size() > stateio::MaxString + 256 || result.entries.size() >= stateio::MaxRecords)
                    throw std::runtime_error("History entry exceeds limits.");
                HistoryEntry entry; std::string path;
                entry.queued = kind == "QUEUED";
                if (!(record >> entry.id >> entry.time >> entry.position >> std::quoted(path)) || entry.id == 0 || entry.id <= previousId ||
                    entry.time < 0 || !std::isfinite(entry.position) || entry.position < 0 || entry.position > 1e12)
                    throw std::runtime_error("Invalid history entry.");
                entry.path = stateio::pathFromText(path); entry.search = searchText(path);
                if (entry.queued && (entry.time != 0 || entry.position != 0)) throw std::runtime_error("Invalid queued visit.");
                previousId = entry.id; result.entries.push_back(std::move(entry));
            } else if (kind == "NEXT_ID" && !nextSeen) {
                if (!(record >> result.nextId) || result.nextId == 0 || result.nextId == UINT64_MAX) throw std::runtime_error("Invalid next visit ID.");
                nextSeen = true;
            } else if (kind == "REPEAT" && !repeatSeen) {
                int mode = 0; if (!(record >> mode)) throw std::runtime_error("Invalid repeat setting.");
                result.repeat = mode >= 0 && mode <= 2 ? static_cast<RepeatMode>(mode) : RepeatMode::Off; repeatSeen = true;
            } else if (kind == "TRAIL" && !trailSeen) {
                uint64_t id = 0; std::set<uint64_t> seen;
                while (record >> id) {
                    if (result.trail.size() >= stateio::MaxRecords || !seen.insert(id).second) throw std::runtime_error("Invalid history trail.");
                    result.trail.push_back(id);
                }
                if (!record.eof()) throw std::runtime_error("Invalid history trail ID.");
                record.clear(); trailSeen = true;
            } else if (kind == "CURSOR" && !cursorSeen) {
                if (!(record >> currentId)) throw std::runtime_error("Invalid history cursor."); cursorSeen = true;
            } else throw std::runtime_error("Invalid history record.");
            record >> std::ws; if (!record.eof()) throw std::runtime_error("Unexpected history data.");
        }
        if (!nextSeen || !trailSeen || !cursorSeen || !repeatSeen || result.nextId <= previousId)
            throw std::runtime_error("Incomplete history file.");
        result.reindex();
        if (result.entries.size() - result.playedCount > 10000) throw std::runtime_error("Saved queue exceeds 10000 tracks.");
        std::erase_if(result.trail, [&](uint64_t id) { return !result.index.contains(id); });
        const auto it = std::find(result.trail.begin(), result.trail.end(), currentId);
        result.cursor = it == result.trail.end() ? (result.trail.empty() ? 0 : result.trail.size() - 1) : static_cast<size_t>(it - result.trail.begin());
        const auto before = result.entries.size();
        result.prune();
        if (result.entries.size() != before) ++result.revision;
        return result;
    }
    static History salvage(const std::string& bytes, size_t cap) {
        // Recover independent valid entries only after strict loading and backup fail.
        // Reconstruct a chronological trail; preserve the damaged source separately.
        if (bytes.size() > stateio::MaxFile)
            throw std::runtime_error("History cannot be safely salvaged.");
        History result; result.limit = cap;
        std::istringstream input(bytes.starts_with("\xef\xbb\xbf") ? bytes.substr(3) : bytes); std::string line;
        if (!std::getline(input, line) || (line != "PRAPPY_HISTORY 1" && line != "PRAPPY_HISTORY 1\r"))
            throw std::runtime_error("History cannot be safely salvaged.");
        uint64_t previous = 0; size_t queuedCount = 0;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if ((!line.starts_with("ENTRY ") && !line.starts_with("QUEUED ")) || line.size() > stateio::MaxString + 256 || result.entries.size() >= stateio::MaxRecords) continue;
            std::istringstream record(line); record.imbue(std::locale::classic());
            std::string kind, path; HistoryEntry entry;
            if (!(record >> kind >> entry.id >> entry.time >> entry.position >> std::quoted(path)) || entry.id <= previous ||
                entry.id >= UINT64_MAX - 1 || entry.time < 0 || !std::isfinite(entry.position) || entry.position < 0 || entry.position > 1e12) continue;
            record >> std::ws; if (!record.eof()) continue;
            entry.queued = kind == "QUEUED";
            if (entry.queued && (entry.time != 0 || entry.position != 0)) continue;
            if (entry.queued && queuedCount >= 10000) continue;
            try { entry.path = stateio::pathFromText(path); entry.search = searchText(path); } catch (const std::exception&) { continue; }
            if (entry.queued) ++queuedCount;
            previous = entry.id; result.trail.push_back(entry.id); result.entries.push_back(std::move(entry));
        }
        if (result.entries.empty()) throw std::runtime_error("No valid history entries to recover.");
        result.nextId = previous + 1; result.cursor = result.trail.size() - 1;
        result.reindex(); result.prune(); ++result.revision; return result;
    }
};
struct HistoryFile {
    std::filesystem::path path;
    stateio::Writer writer;
    uint64_t saved = 0;
    bool writable = true;
    bool urgent = false;
    std::string notice;
    std::chrono::steady_clock::time_point nextSave{};
    explicit HistoryFile(std::filesystem::path file) : path(std::move(file)) {}
    History load(size_t limit) {
        History empty; empty.limit = limit;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !std::filesystem::exists(path.wstring() + L".bak", ec)) return empty;
        try { return History::parse(stateio::read(path), limit); }
        catch (const stateio::FutureVersion& e) { writable = false; notice = e.what(); return empty; }
        catch (const std::exception& e) { notice = e.what(); }
        writer.backup = false;
        try {
            auto result = History::parse(stateio::read(path.wstring() + L".bak"), limit);
            ++result.revision; notice = "Recovered history backup: " + notice; return result;
        } catch (const std::exception&) {
            try {
                const auto damaged = stateio::read(path);
                auto result = History::salvage(damaged, limit);
                const auto preserved = path.wstring() + L".damaged-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
                stateio::write(preserved, damaged, false);
                notice = "Recovered valid history entries; damaged original preserved.";
                return result;
            } catch (const std::exception&) {
                writable = false; notice = "Cannot recover history; original files preserved. " + notice;
                return empty;
            }
        }
    }
    void save(History& history, bool immediate = false, bool finish = false) {
        if (immediate) urgent = true;
        const auto now = std::chrono::steady_clock::now();
        if (writer.collect(finish)) {
            if (writer.error.empty()) { saved = writer.revision; notice.clear(); }
            else { notice = writer.error; nextSave = now + std::chrono::seconds(5); }
        }
        if (!writable || writer.busy() || history.revision == saved || (!finish && !urgent && now < nextSave)) return;
        if (!finish && !writer.error.empty() && now < nextSave) return;
        try {
            writer.startGenerated(path, [snapshot = history.snapshot()] { return snapshot.serialize(); }, history.revision);
            urgent = false;
            nextSave = now + std::chrono::seconds(15);
        } catch (const std::exception& e) { notice = e.what(); nextSave = now + std::chrono::seconds(5); }
        if (finish && writer.collect(true)) {
            if (writer.error.empty()) { saved = writer.revision; notice.clear(); } else notice = writer.error;
        }
    }
};

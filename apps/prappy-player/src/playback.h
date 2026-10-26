#pragma once
#include "audio.h"
#include "library.h"
#include "history.h"
// Playback intents are committed by matching successful mpv playlist entry IDs.
struct Playback {
    enum class Kind { NewVisit, Traverse, Restore, Restart, Rollback };
    struct Pending { Kind kind; uint64_t serial; uint64_t targetId; bool oldPaused; double position; bool paused; };
    Audio& audio;
    Library& library;
    History& history;
    HistoryFile& file;
    std::optional<Pending> pending;
    std::set<std::wstring> unavailable;
    std::string notice;
    uint64_t generation = 0;
    Playback(Audio& source, Library& catalog, History& visits, HistoryFile& storage)
        : audio(source), library(catalog), history(visits), file(storage) {}
    ~Playback() { try { finish(); } catch (const std::exception& e) { OutputDebugStringA(e.what()); } }
    void checkpoint(bool complete = false) {
        const auto entry = history.current();
        if (!pending && entry && key(entry->path) == key(audio.current) && (audio.loaded || complete))
            history.position(complete ? 0 : std::max(0.0, audio.position));
    }
    bool request(const fs::path& path, Kind kind = Kind::NewVisit, size_t target = 0, bool paused = false, double position = 0) {
        checkpoint();
        const bool oldPaused = pending ? pending->oldPaused : audio.paused;
        const uint64_t targetId = kind != Kind::NewVisit && target < history.trail.size() ? history.trail[target] : 0;
        // Load paused, then honor the latest play/pause intent after committing.
        // This also prevents a brief audible start when restoring a saved session.
        if (!audio.play(path, true, kind == Kind::Restore ? 0 : position)) { notice = audio.error; return false; }
        pending = Pending{kind, audio.serial, targetId, oldPaused, position, paused};
        notice.clear(); ++generation; return true;
    }
    void restore() {
        if (const auto entry = history.current()) request(entry->path, Kind::Restore, history.cursor, true, entry->position);
    }
    std::optional<size_t> adjacent(int direction) const {
        auto cursor = history.cursor;
        if (pending && pending->kind == Kind::Traverse) {
            const auto it = std::find(history.trail.begin(), history.trail.end(), pending->targetId);
            if (it != history.trail.end()) cursor = static_cast<size_t>(it - history.trail.begin());
        }
        if (history.trail.empty() || (direction < 0 && cursor == 0) || (direction > 0 && cursor + 1 >= history.trail.size())) return {};
        return direction < 0 ? cursor - 1 : cursor + 1;
    }
    fs::path libraryNext(bool automatic) const {
        if (library.tracks.empty()) return {};
        const auto id = key(audio.current);
        auto it = std::find_if(library.tracks.begin(), library.tracks.end(), [&](const auto& track) { return Library::matches(track, id); });
        size_t index = it == library.tracks.end() ? 0 : static_cast<size_t>(it - library.tracks.begin()) + 1;
        if (index >= library.tracks.size()) {
            if (automatic && history.repeat != RepeatMode::All) return {};
            index = 0;
        }
        return library.tracks[index].path;
    }
    fs::path destination(int direction) const {
        if (auto target = adjacent(direction)) return history.find(history.trail[*target])->path;
        return direction > 0 ? libraryNext(false) : fs::path{};
    }
    bool navigate(int direction, bool automatic = false) {
        if (automatic && history.repeat == RepeatMode::One && history.current())
            return request(history.current()->path, Kind::Restart, history.cursor);
        if (const auto target = adjacent(direction))
            return request(history.find(history.trail[*target])->path, Kind::Traverse, *target);
        if (direction < 0) return false;
        const auto path = libraryNext(automatic);
        return !path.empty() && request(path);
    }
    void toggle() {
        if (pending) pending->paused = !pending->paused;
        else if (audio.active) { audio.pause(); checkpoint(); file.save(history, true); }
        else if (const auto entry = history.current()) request(entry->path, Kind::Restart, history.cursor);
        else if (!audio.current.empty()) request(audio.current);
        else navigate(1);
    }
    void tick() {
        const bool eof = audio.poll();
        if (pending && audio.loadedSerial == pending->serial) {
            const auto completed = *pending; pending.reset();
            unavailable.erase(key(audio.current));
            const auto destination = std::find(history.trail.begin(), history.trail.end(), completed.targetId);
            if (completed.kind == Kind::NewVisit || destination == history.trail.end()) history.add(audio.current);
            else history.visit(static_cast<size_t>(destination - history.trail.begin()));
            if (completed.kind == Kind::Restore) {
                const double position = audio.duration > .25 && completed.position < audio.duration - .1 ? completed.position : 0;
                audio.seek(position); audio.position = position; history.position(position);
            }
            audio.setPaused(completed.paused);
            ++generation; file.save(history, true);
        }
        if (pending && !audio.error.empty()) {
            const auto failed = *pending; pending.reset();
            unavailable.insert(key(audio.current)); notice = audio.error; ++generation;
            if (failed.kind != Kind::Rollback && failed.kind != Kind::Restore) {
                if (const auto previous = history.current()) {
                    const auto message = notice;
                    request(previous->path, Kind::Rollback, history.cursor, failed.oldPaused, previous->position);
                    notice = message;
                }
            }
        }
        checkpoint(eof);
        if (eof && !pending) navigate(1, true);
        file.save(history);
    }
    void finish() {
        checkpoint(); file.save(history, true, true);
        if (!file.notice.empty()) OutputDebugStringA((file.notice + "\n").c_str());
    }
};

#pragma once
#include "paths.h"
#include <mpv/client.h>
#include <array>
#include <vector>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <cstdint>
struct AudioInfo {
    std::string codec, sampleFormat, channelLayout;
    int64_t sampleRate = 0, channels = 0;
    double bitrate = 0;
    std::string describe() const {
        if (codec.empty()) return "Audio format unavailable";
        auto result = codec;
        if (sampleRate > 0) result += " · " + std::to_string(sampleRate) + " Hz";
        if (channels > 0) result += " · " + std::to_string(channels) + " ch";
        if (!channelLayout.empty()) result += " (" + channelLayout + ")";
        if (!sampleFormat.empty()) result += " · decoded " + sampleFormat;
        if (std::isfinite(bitrate) && bitrate > 0 && bitrate < 1e12)
            result += " · " + std::to_string(static_cast<int64_t>(bitrate / 1000)) + " kb/s";
        return result;
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
    AudioInfo info;
    ~Audio() { if (handle) terminate_destroy(handle); if (dll) FreeLibrary(dll); }
    void open(bool silent = false, std::initializer_list<std::pair<const char*, const char*>> options = {}) {
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
        for (const auto& [name, value] : options) check(set_option_string(handle, name, value));
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
        info = {};
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
    std::string textProperty(const char* name) const {
        char* value = nullptr;
        if (get_property(handle, name, MPV_FORMAT_STRING, &value) < 0) return {};
        std::string result = value ? value : ""; free(value); return result;
    }
    void readInfo() {
        info.codec = textProperty("current-tracks/audio/codec");
        info.sampleFormat = textProperty("audio-params/format");
        info.channelLayout = textProperty("audio-params/channels");
        get_property(handle, "audio-params/samplerate", MPV_FORMAT_INT64, &info.sampleRate);
        get_property(handle, "audio-params/channel-count", MPV_FORMAT_INT64, &info.channels);
        get_property(handle, "current-tracks/audio/demux-bitrate", MPV_FORMAT_DOUBLE, &info.bitrate);
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
                readInfo();
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

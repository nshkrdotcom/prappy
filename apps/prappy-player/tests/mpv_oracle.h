#pragma once
// Independent decoder oracle, linked only into explicit regression executables.
#include "paths.h"
#include <mpv/client.h>
#include <array>
#include <vector>
#include <memory>
#include <chrono>
#include <cmath>
#include <functional>
struct OracleInfo{std::string codec;};
struct MpvOracle {
    HMODULE dll=nullptr;mpv_handle* handle=nullptr;
#define MPV_API(name) decltype(&mpv_##name) name=nullptr
    MPV_API(create);MPV_API(initialize);MPV_API(terminate_destroy);MPV_API(set_option_string);
    MPV_API(command);MPV_API(set_property);MPV_API(get_property);MPV_API(wait_event);MPV_API(error_string);MPV_API(free);
    MPV_API(observe_property);MPV_API(free_node_contents);
#undef MPV_API
    std::string error;fs::path current;OracleInfo info;
    bool loaded=false,paused=false,active=false,waitingStart=false,pauseRequested=false,sequentialRetry=false;
    int64_t desiredEntry=-1,eventEntry=-1;uint64_t serial=0,loadedSerial=0;
    double position=0,duration=0,volume=100,gain=0,meterPeak=0;
    using Clock=std::chrono::steady_clock;Clock::time_point loadStarted{},positionTime{};
    ~MpvOracle(){if(handle)terminate_destroy(handle);if(dll)FreeLibrary(dll);}
    void open(bool silent = false, std::initializer_list<std::pair<const char*, const char*>> options = {}) {
        if (handle || dll) throw std::logic_error("Audio is already open.");

        auto path = executableDir() / "libmpv-2.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw std::runtime_error("Cannot load libmpv-2.dll (Windows error " + std::to_string(GetLastError()) + ").");
#define MPV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, "mpv_" #name)); if (!name) throw std::runtime_error("Incompatible libmpv: " #name)
        MPV_LOAD(create); MPV_LOAD(initialize); MPV_LOAD(terminate_destroy); MPV_LOAD(set_option_string);
        MPV_LOAD(command); MPV_LOAD(set_property); MPV_LOAD(get_property); MPV_LOAD(wait_event); MPV_LOAD(error_string); MPV_LOAD(free);
        MPV_LOAD(observe_property); MPV_LOAD(free_node_contents);
#undef MPV_LOAD
        handle = create(); if (!handle) throw std::runtime_error("libmpv could not create the player.");
        for (auto [name, value] : std::array<std::pair<const char*, const char*>, 8>{{
            {"config", "no"}, {"video", "no"}, {"audio-display", "no"}, {"terminal", "no"},
            {"input-default-bindings", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"idle", "yes"}}})
            check(set_option_string(handle, name, value));
        if (silent) check(set_option_string(handle, "ao", "null"));
        check(set_option_string(handle, "replaygain", "no")); // Application policy is the only optional loudness gain.
        check(set_option_string(handle, "audio-channels", "stereo"));
        check(set_option_string(handle, "audio-samplerate", "48000"));
        for (const auto& [name, value] : options) check(set_option_string(handle, name, value));
        check(initialize(handle)); check(set_property(handle, "volume", MPV_FORMAT_DOUBLE, &volume));
        check(observe_property(handle, 1, "time-pos", MPV_FORMAT_DOUBLE));
        check(observe_property(handle, 2, "duration", MPV_FORMAT_DOUBLE));
        check(observe_property(handle, 3, "pause", MPV_FORMAT_FLAG));

    }
    void check(int result) { if (result < 0) throw std::runtime_error(error_string(result)); }
    bool run(std::initializer_list<const char*> args) {

        std::vector<const char*> list(args); list.push_back(nullptr);
        int result = command(handle, list.data());
        if (result < 0) { error = error_string(result); return false; } return true;
    }
    static std::string decoderFile(fs::path nativePath) {
        nativePath.make_preferred();
        // Keep the caller's source identity unchanged, but give mpv a native
        // extended path so valid Windows/WSL filenames can exceed MAX_PATH.
        return utf8(nativePath.is_absolute() ? extendedPath(nativePath) : nativePath);
    }
    bool play(const fs::path& path, bool startPaused = false, double startPosition = 0) {

        loadStarted=Clock::now();

        poll(); // Consume the previous track's events before replacing its identity.
        const auto file=decoderFile(path);
        int pauseValue = startPaused ? 1 : 0;
        const int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &pauseValue);
        if (result < 0) { error = error_string(result); return false; }
        setGain(0); meterPeak = 0;
        const auto options = std::string(startPaused ? "pause=yes,start=" : "pause=no,start=") + std::to_string(std::max(0.0, startPosition));
        if (!run({"loadfile", file.c_str(), "replace", "-1", options.c_str()})) return false;

        if (get_property(handle, "playlist/0/id", MPV_FORMAT_INT64, &desiredEntry) < 0) {
            error = "Cannot identify requested playback."; return false;
        }
        ++serial;
        sequentialRetry = false;
        current = path; loaded = false; active = true; waitingStart = true;
        info = {};
        position = startPosition; positionTime = Clock::now(); duration = 0; paused = pauseRequested = startPaused; error.clear(); return true;
    }
    bool retryEmptyFlac(const mpv_event_end_file& end) {
        if (end.playlist_entry_id != desiredEntry || end.reason != MPV_END_FILE_REASON_ERROR ||
            end.error != MPV_ERROR_NOTHING_TO_PLAY || sequentialRetry || key(current.extension()) != L".flac") return false;
        // A single-frame FLAC can fail libavformat's initial seek before any
        // samples reach the decoder. Retry once as a sequential stream, keeping
        // a bounded packet cache and the same application playback intent.
        sequentialRetry = true;
        const auto stream = "ffmpeg://"+decoderFile(current);
        const auto options = std::string(pauseRequested ? "pause=yes" : "pause=no")+
            ",stream-lavf-o=seekable=0,demuxer-seekable-cache=yes,demuxer-max-bytes=1048576";
        if (!run({"loadfile",stream.c_str(),"replace","-1",options.c_str()})) return false;
        if (get_property(handle,"playlist/0/id",MPV_FORMAT_INT64,&desiredEntry) < 0) return false;
        loaded = false; loadedSerial = 0; active = true; waitingStart = true;
        info = {}; position = duration = 0;
        positionTime = Clock::now(); error.clear(); return true;
    }
    void setGain(double value) {
        if (!std::isfinite(value)) return;
        value = std::clamp(value, -96.0, 12.0);

        const auto result = set_property(handle, "volume-gain", MPV_FORMAT_DOUBLE, &value);
        if (result < 0) error = error_string(result); else gain = value;
    }
    std::string textProperty(const char* name) const {

        char* value = nullptr;
        if (get_property(handle, name, MPV_FORMAT_STRING, &value) < 0) return {};
        const std::unique_ptr<char, decltype(free)> owner(value, free);
        return value ? value : "";
    }
    std::vector<std::pair<std::string, std::string>> loadedTags() const {

        std::vector<std::pair<std::string, std::string>> result;
        if (!handle || !loaded || !free_node_contents) return result;
        mpv_node metadata{};
        if (get_property(handle, "metadata", MPV_FORMAT_NODE, &metadata) < 0) return result;
        struct NodeOwner {
            const MpvOracle* audio; mpv_node& value;
            ~NodeOwner() { audio->free_node_contents(&value); }
        } owner{this, metadata};
        return tagsFromNode(metadata);
    }
    static std::vector<std::pair<std::string, std::string>> tagsFromNode(const mpv_node& metadata) {
        std::vector<std::pair<std::string, std::string>> result;
        if (metadata.format != MPV_FORMAT_NODE_MAP || !metadata.u.list) return result;
        const auto* list = metadata.u.list;
        if (list->num < 0 || list->num > 2048 || (list->num && (!list->keys || !list->values))) return result;
        for (int i = 0; i < list->num; ++i) {
            if (!list->keys[i] || list->values[i].format != MPV_FORMAT_STRING || !list->values[i].u.string) continue;
            std::string key(list->keys[i]);
            std::string value(list->values[i].u.string);
            if (key.size() > 256 || value.size() > 4096) continue;
            result.emplace_back(std::move(key), std::move(value));
        }
        return result;
    }
    void readInfo() {
        info.codec = textProperty("current-tracks/audio/codec");
    }
    bool poll() {

        bool eof = false;
        for (;;) {
            auto event = wait_event(handle, 0);
            if (event->event_id == MPV_EVENT_NONE) break;
            if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                const auto property = static_cast<mpv_event_property*>(event->data);
                if (event->reply_userdata == 1 && property->format == MPV_FORMAT_DOUBLE) {
                    position = *static_cast<double*>(property->data); positionTime = Clock::now();
                }
                if (event->reply_userdata == 2 && property->format == MPV_FORMAT_DOUBLE) duration = *static_cast<double*>(property->data);
                if (event->reply_userdata == 3 && property->format == MPV_FORMAT_FLAG) paused = *static_cast<int*>(property->data) != 0;

            }
            if (event->event_id == MPV_EVENT_START_FILE) {
                eventEntry = static_cast<mpv_event_start_file*>(event->data)->playlist_entry_id;
                if (eventEntry == desiredEntry) { waitingStart = false; error.clear(); }
            }
            if (event->event_id == MPV_EVENT_FILE_LOADED && eventEntry == desiredEntry) {
                loaded = true; active = true; loadedSerial = serial;

                readInfo();
            }
            if (event->event_id == MPV_EVENT_AUDIO_RECONFIG && loaded && eventEntry == desiredEntry) readInfo();
            if (event->event_id == MPV_EVENT_END_FILE) {
                auto end = static_cast<mpv_event_end_file*>(event->data);
                if (end->playlist_entry_id != desiredEntry) continue;
                if (retryEmptyFlac(*end)) continue;
                if (end->reason == MPV_END_FILE_REASON_ERROR) error = "Playback: " + std::string(error_string(end->error));
                if (end->reason == MPV_END_FILE_REASON_EOF) { eof = true; active = false; loaded = false; }
                if (end->reason == MPV_END_FILE_REASON_ERROR) { active = false; loaded = false; }
                    }
        }
        if (loaded) {
            // Decoder parameters can arrive after FILE_LOADED, especially on a paused restore.
            if(info.codec.empty())readInfo();

        }

        return eof;
    }
};

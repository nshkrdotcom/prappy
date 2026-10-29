#pragma once
#include "paths.h"
#include "output_meter.h"
#include <mpv/client.h>
#include <array>
#include <vector>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <cstdint>
#include <charconv>
#include <functional>
#include <optional>
#include <chrono>
#include <memory>
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
    MPV_API(set_wakeup_callback); MPV_API(command_async); MPV_API(observe_property);
#undef MPV_API
    std::string error;
    fs::path current;
    bool loaded = false, paused = false, active = false, waitingStart = false;
    bool pauseRequested = false; // User intent survives mpv's file-option reset at EOF.
    int64_t desiredEntry = -1, eventEntry = -1;
    uint64_t serial = 0, loadedSerial = 0;
    double position = 0, duration = 0, volume = 70;
    AudioInfo info;
    double gain = 0, meterPeak = 0;
    std::unique_ptr<OutputMeter> outputMeter;
    bool meterAvailable = false;
    bool sequentialRetry = false;
    using Clock = std::chrono::steady_clock;
    Clock::time_point positionTime = Clock::now();
    std::optional<double> queuedSeek, seekPreview;
    uint64_t seekSequence = 0, pendingSeek = 0;
    double displayPosition(Clock::time_point now = Clock::now()) const {
        if (seekPreview) return *seekPreview;
        const auto elapsed = loaded && active && !paused ? std::chrono::duration<double>(now-positionTime).count() : 0;
        return std::clamp(position+std::max(0.0,elapsed),0.0,std::max(0.0,duration));
    }
    std::function<void()> notify;
    ~Audio() { if (handle) { set_wakeup_callback(handle, nullptr, nullptr); terminate_destroy(handle); } if (dll) FreeLibrary(dll); }
    void setNotify(std::function<void()> callback) {
        notify = std::move(callback);
        set_wakeup_callback(handle, [](void* context) { auto& audio = *static_cast<Audio*>(context); if (audio.notify) audio.notify(); }, this);
    }
    void open(bool silent = false, std::initializer_list<std::pair<const char*, const char*>> options = {}, bool meter = true) {
        if (dll) throw std::logic_error("Audio is already open.");
        auto path = executableDir() / "libmpv-2.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw std::runtime_error("Cannot load libmpv-2.dll (Windows error " + std::to_string(GetLastError()) + ").");
#define MPV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, "mpv_" #name)); if (!name) throw std::runtime_error("Incompatible libmpv: " #name)
        MPV_LOAD(create); MPV_LOAD(initialize); MPV_LOAD(terminate_destroy); MPV_LOAD(set_option_string);
        MPV_LOAD(command); MPV_LOAD(set_property); MPV_LOAD(get_property); MPV_LOAD(wait_event); MPV_LOAD(error_string); MPV_LOAD(free);
        MPV_LOAD(set_wakeup_callback); MPV_LOAD(command_async); MPV_LOAD(observe_property);
#undef MPV_LOAD
        handle = create(); if (!handle) throw std::runtime_error("libmpv could not create the player.");
        for (auto [name, value] : std::array<std::pair<const char*, const char*>, 8>{{
            {"config", "no"}, {"video", "no"}, {"audio-display", "no"}, {"terminal", "no"},
            {"input-default-bindings", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"idle", "yes"}}})
            check(set_option_string(handle, name, value));
        if (silent) check(set_option_string(handle, "ao", "null"));
        check(set_option_string(handle, "audio-channels", "stereo"));
        check(set_option_string(handle, "audio-samplerate", "48000"));
        for (const auto& [name, value] : options) check(set_option_string(handle, name, value));
        check(initialize(handle)); check(set_property(handle, "volume", MPV_FORMAT_DOUBLE, &volume));
        check(observe_property(handle, 1, "time-pos", MPV_FORMAT_DOUBLE));
        check(observe_property(handle, 2, "duration", MPV_FORMAT_DOUBLE));
        check(observe_property(handle, 3, "pause", MPV_FORMAT_FLAG));
        if (meter && !silent) outputMeter = std::make_unique<OutputMeter>();
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
        setGain(0); meterPeak = 0;
        const auto options = std::string(startPaused ? "pause=yes,start=" : "pause=no,start=") + std::to_string(std::max(0.0, startPosition));
        if (!run({"loadfile", file.c_str(), "replace", "-1", options.c_str()})) return false;
        if (get_property(handle, "playlist/0/id", MPV_FORMAT_INT64, &desiredEntry) < 0) {
            error = "Cannot identify requested playback."; return false;
        }
        ++serial;
        sequentialRetry = false;
        queuedSeek.reset(); seekPreview.reset(); pendingSeek = 0;
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
        const auto stream = "ffmpeg://"+utf8(current);
        const auto options = std::string(pauseRequested ? "pause=yes" : "pause=no")+
            ",stream-lavf-o=seekable=0,demuxer-seekable-cache=yes,demuxer-max-bytes=1048576";
        if (!run({"loadfile",stream.c_str(),"replace","-1",options.c_str()})) return false;
        if (get_property(handle,"playlist/0/id",MPV_FORMAT_INT64,&desiredEntry) < 0) return false;
        loaded = false; loadedSerial = 0; active = true; waitingStart = true;
        queuedSeek.reset(); seekPreview.reset(); info = {}; position = duration = 0;
        positionTime = Clock::now(); error.clear(); return true;
    }
    void setPaused(bool value) {
        if (!active) { paused = pauseRequested = value; return; }
        int flag = value ? 1 : 0;
        int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &flag);
        if (result < 0) error = error_string(result); else {
            position = displayPosition(); positionTime = Clock::now(); paused = pauseRequested = value;
            if (!value) seekPreview.reset();
        }
    }
    void pause() { setPaused(!paused); }
    void seek(double seconds) {
        if (!loaded || !std::isfinite(seconds)) return;
        const auto target = std::max(0.0,seconds);
        queuedSeek = seekPreview = duration > 0 ? std::min(target,duration) : target;
        submitSeek();
    }
    void submitSeek() {
        if (pendingSeek || !queuedSeek) return;
        const auto value = std::to_string(*queuedSeek);
        const char* args[] = {"seek",value.c_str(),"absolute+exact",nullptr};
        pendingSeek = ++seekSequence; queuedSeek.reset();
        const auto result = command_async(handle,pendingSeek,args);
        if (result < 0) { error = error_string(result); pendingSeek = 0; seekPreview.reset(); }
    }
    void setVolume(double value) {
        int result = set_property(handle, "volume", MPV_FORMAT_DOUBLE, &value);
        if (result < 0) error = error_string(result); else volume = value;
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
            if (event->event_id == MPV_EVENT_COMMAND_REPLY && pendingSeek && event->reply_userdata == pendingSeek) {
                pendingSeek = 0;
                if (event->error < 0) { error = error_string(event->error); if (!queuedSeek) seekPreview.reset(); }
            }
            if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                const auto property = static_cast<mpv_event_property*>(event->data);
                if (event->reply_userdata == 1 && property->format == MPV_FORMAT_DOUBLE) {
                    position = *static_cast<double*>(property->data); positionTime = Clock::now();
                    // Paused audio timestamps can precede the exact requested sample.
                    // Keep the clicked marker stable until playback resumes.
                    if (seekPreview && !queuedSeek && !paused && std::abs(position-*seekPreview) < .25) seekPreview.reset();
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
                queuedSeek.reset(); seekPreview.reset(); pendingSeek = 0;
            }
        }
        if (loaded) {
            // Decoder parameters can arrive after FILE_LOADED, especially on a paused restore.
            if (info.codec.empty() || info.sampleFormat.empty() || info.sampleRate <= 0 || info.channels <= 0) readInfo();
            submitSeek();
        }
        if (outputMeter) {
            const bool audible = loaded && active && !paused;
            outputMeter->enable(audible);
            const auto peak = outputMeter->read();
            meterPeak = audible ? peak : 0;
            meterAvailable = audible && outputMeter->available();
        } else { meterPeak = 0; meterAvailable = false; }
        return eof;
    }
};

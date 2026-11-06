#pragma once
#include "paths.h"
#include "debug_log.h"
#include "output_meter.h"
#include "native_playback.h"
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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
namespace prepared {struct CatalogSource;}
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
    bool nativeMode=false;
    std::optional<sampling::PcmBudget> nativeMemory;
    std::shared_ptr<sampling::NativePlayback> native;
    bool nativeEditing=false;
    sampling::Frame nativeReturnFrame=0;
    sampling::Connection nativeConnection;
    HMODULE dll = nullptr;
    mpv_handle* handle = nullptr;
#define MPV_API(name) decltype(&mpv_##name) name = nullptr
    MPV_API(create); MPV_API(initialize); MPV_API(terminate_destroy); MPV_API(set_option_string);
    MPV_API(command); MPV_API(set_property); MPV_API(get_property); MPV_API(wait_event); MPV_API(error_string); MPV_API(free);
    MPV_API(set_wakeup_callback); MPV_API(command_async); MPV_API(observe_property); MPV_API(free_node_contents);
#undef MPV_API
    struct Async;
    std::shared_ptr<Async> worker;
    std::vector<std::pair<std::string,std::string>> cachedTags;
    static void checkWorkerCapacity();
    void startWorker();
    void stopWorker();
    void setWorkerNotify();
    void enqueue(int kind, std::function<void(Audio&)> action, bool replace = false);
    bool collectWorker();
    // Explicit inspection barrier for native tests; never called by UI code.
    void waitIdle();
    std::string error;
    fs::path current;
    std::shared_ptr<const prepared::CatalogSource> preparedSource;
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
    bool logPlayback = false;
    using Clock = std::chrono::steady_clock;
    Clock::time_point positionTime = Clock::now();
    Clock::time_point loadStarted = Clock::now();
    std::optional<double> queuedSeek, seekPreview;
    uint64_t seekSequence = 0, pendingSeek = 0;
    double displayPosition(Clock::time_point now = Clock::now()) const {
        if (seekPreview) return *seekPreview;
        if(nativeMode)return position;
        const auto elapsed = loaded && active && !paused ? std::chrono::duration<double>(now-positionTime).count() : 0;
        return std::clamp(position+std::max(0.0,elapsed),0.0,std::max(0.0,duration));
    }
    std::function<void()> notify;
    ~Audio() { stopWorker(); if (handle) { set_wakeup_callback(handle, nullptr, nullptr); terminate_destroy(handle); } if (dll) FreeLibrary(dll); }
    void setNotify(std::function<void()> callback) {
        notify = std::move(callback);
        if(nativeMode)return;
        if (worker) { setWorkerNotify(); return; }
        set_wakeup_callback(handle, [](void* context) { auto& audio = *static_cast<Audio*>(context); if (audio.notify) audio.notify(); }, this);
    }
    void openNative(bool silent,sampling::PcmBudget memory){
        if(handle||dll||worker||nativeMode)throw std::logic_error("Audio is already open.");
        nativeMode=true;nativeMemory=std::move(memory);logPlayback=true;
        if(silent)SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
        else outputMeter=std::make_unique<OutputMeter>();
    }
    bool playNative(const fs::path& path,const prepared::CatalogSource& source,const sampling::Connection& backend,bool startPaused,double startPosition){
        if(!nativeMode)throw std::logic_error("Catalog playback requires the native transport.");
        nativeEditing=false;loadStarted=Clock::now();++serial;current=path;loaded=false;loadedSerial=0;active=true;info={};
        paused=pauseRequested=startPaused;position=startPosition;duration=source.duration;error.clear();gain=0;
        queuedSeek.reset();seekPreview.reset();pendingSeek=0;cachedTags=source.tags;
        preparedSource=std::make_shared<prepared::CatalogSource>(source);
        if(logPlayback)debuglog::write("play.request","serial="+std::to_string(serial)+" source="+utf8(path));
        try{
            if(!native||nativeConnection.url!=backend.url||nativeConnection.token!=backend.token||nativeConnection.credentialFile!=backend.credentialFile){
                native.reset();nativeConnection=backend;
                native=std::make_shared<sampling::NativePlayback>(backend,*nativeMemory,notify);
            }
            native->load(source.source.endpoint(),source.source.capture,source.asset,startPaused,startPosition);
            native->gain(static_cast<float>(volume/100));return true;
        }catch(const std::exception& e){error=e.what();active=loaded=false;return false;}
    }
    std::shared_ptr<sampling::NativePlayback> takeNativeForEditor(){
        if(!nativeMode||!native||!active)return {};
        native->pause(true);native->tick();const auto& state=native->state();
        nativeReturnFrame=state.rate>0?state.cursor:-1;
        if(state.rate>0)position=static_cast<double>(nativeReturnFrame)/state.rate;
        paused=pauseRequested=true;nativeEditing=true;return native;
    }
    void resumeNative(){
        if(!nativeEditing)return;
        nativeEditing=false;
        if(!native||!preparedSource)return;
        const auto& source=*preparedSource;
        if(native->reference()==source.source.endpoint()&&(!native->descriptor().is_null()||native->state().buffering)){
            const auto& d=native->descriptor();
            if(!d.is_null())native->start({0,prepared::integer(d.at("frame_count"))},nativeReturnFrame>=0?nativeReturnFrame:
                static_cast<sampling::Frame>(std::llround(position*prepared::integer(d.at("sample_rate")))),false,true);
            native->pause(true);native->clearCache();
        }else native->load(source.source.endpoint(),source.source.capture,source.asset,true,position);
        native->gain(static_cast<float>(volume/100*std::pow(10.,gain/20)));
    }
    void updateMeter(){
        const bool audible=loaded&&active&&!paused&&volume>0;
        if(outputMeter){outputMeter->enable(audible);meterPeak=audible?outputMeter->read():0;meterAvailable=audible&&outputMeter->available();}
        else{meterPeak=0;meterAvailable=false;}
    }
    bool pollNative(){
        if(nativeEditing||!native||current.empty()){updateMeter();return false;}
        native->tick();const auto& state=native->state();
        if(!state.error.empty()){error=state.error;loaded=active=false;updateMeter();return false;}
        const bool eof=state.finished&&active;
        if(state.rate>0){position=static_cast<double>(state.cursor)/state.rate;duration=static_cast<double>(state.frames)/state.rate;}
        paused=state.paused;loaded=state.loaded&&!state.finished;active=!state.finished;
        if(loaded&&loadedSerial!=serial){
            loadedSerial=serial;const auto& descriptor=native->descriptor();
            auto source=std::make_shared<prepared::CatalogSource>(*preparedSource);source->timeline=descriptor.at("timeline_id");
            source->native={state.rate,prepared::integer(descriptor.at("channels")),state.frames,prepared::sampleType(descriptor.at("sample_format").get<std::string>())};
            info.codec=source->codec;info.sampleRate=state.rate;info.channels=source->native.channels;info.sampleFormat=descriptor.at("sample_format");preparedSource=std::move(source);
            if(logPlayback)debuglog::write("play.loaded","serial="+std::to_string(serial)+" ms="+std::to_string(debuglog::elapsed(loadStarted))+" native_rate="+std::to_string(state.rate));
        }
        if(pendingSeek&&!state.buffering){pendingSeek=0;seekPreview.reset();submitSeek();}
        updateMeter();return eof;
    }
    void open(bool silent = false, std::initializer_list<std::pair<const char*, const char*>> options = {}, bool meter = true) {
        (void)debuglog::instance(); // Outlive playback actors during static teardown.
        logPlayback=meter;
        if (handle || dll) throw std::logic_error("Audio is already open.");
        if (meter) checkWorkerCapacity();
        auto path = executableDir() / "libmpv-2.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw std::runtime_error("Cannot load libmpv-2.dll (Windows error " + std::to_string(GetLastError()) + ").");
#define MPV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, "mpv_" #name)); if (!name) throw std::runtime_error("Incompatible libmpv: " #name)
        MPV_LOAD(create); MPV_LOAD(initialize); MPV_LOAD(terminate_destroy); MPV_LOAD(set_option_string);
        MPV_LOAD(command); MPV_LOAD(set_property); MPV_LOAD(get_property); MPV_LOAD(wait_event); MPV_LOAD(error_string); MPV_LOAD(free);
        MPV_LOAD(set_wakeup_callback); MPV_LOAD(command_async); MPV_LOAD(observe_property); MPV_LOAD(free_node_contents);
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
        if (meter && !silent) outputMeter = std::make_unique<OutputMeter>();
        if (meter) startWorker();
    }
    void check(int result) { if (result < 0) throw std::runtime_error(error_string(result)); }
    bool run(std::initializer_list<const char*> args) {
        if(nativeMode){
            if(args.size()==1&&std::string_view(*args.begin())=="stop"){if(native)native->stop();loaded=active=false;current.clear();preparedSource.reset();queuedSeek.reset();seekPreview.reset();pendingSeek=0;return true;}
            throw std::logic_error("Unsupported native playback command.");
        }
        if (worker) {
            std::vector<std::string> owned; for (const auto* arg : args) owned.emplace_back(arg);
            enqueue(5,[owned=std::move(owned)](Audio& core){ std::vector<const char*> values; for(const auto& arg:owned)values.push_back(arg.c_str()); values.push_back(nullptr); const auto result=core.command(core.handle,values.data());if(result<0)core.error=core.error_string(result);else if(!owned.empty()&&owned[0]=="stop"){core.loaded=core.active=false;core.loadedSerial=0;core.desiredEntry=core.eventEntry=-1;core.current.clear();core.queuedSeek.reset();core.seekPreview.reset();core.pendingSeek=0;} });
            return true;
        }
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
    bool play(const fs::path& path, bool startPaused = false, double startPosition = 0,
              const std::string& streamUrl = {},const std::string& bearerToken = {}) {
        if(nativeMode)throw std::logic_error("Native playback requires an indexed source.");
        loadStarted=Clock::now();
        preparedSource.reset();
        if (worker) {
            ++serial; const auto epoch=serial; const auto level=volume;
            if(logPlayback)debuglog::write("play.request","serial="+std::to_string(serial)+" source="+utf8(path));
            current=path; loaded=false; loadedSerial=0; active=true; waitingStart=true; info={}; cachedTags.clear();
            queuedSeek.reset();seekPreview.reset();pendingSeek=0; gain=0;meterPeak=0;
            position=startPosition;positionTime=Clock::now();duration=0;paused=pauseRequested=startPaused;error.clear();
            enqueue(0,[path,startPaused,startPosition,epoch,level,streamUrl,bearerToken](Audio& core){core.serial=epoch-1;core.setVolume(level); if(!core.play(path,startPaused,startPosition,streamUrl,bearerToken)){core.serial=epoch;core.current=path;core.active=core.loaded=false;core.loadedSerial=0;core.waitingStart=false;}},true);
            return true;
        }
        poll(); // Consume the previous track's events before replacing its identity.
        const auto file = streamUrl.empty()?decoderFile(path):streamUrl;
        const auto header=bearerToken.empty()?std::string{}:"Authorization: Bearer "+bearerToken;
        const char* headerValue=header.c_str();
        const auto headerStatus=set_property(handle,"http-header-fields",MPV_FORMAT_STRING,&headerValue);
        if(headerStatus<0){error=error_string(headerStatus);return false;}
        int pauseValue = startPaused ? 1 : 0;
        const int result = set_property(handle, "pause", MPV_FORMAT_FLAG, &pauseValue);
        if (result < 0) { error = error_string(result); return false; }
        setGain(0); meterPeak = 0;
        const auto options = std::string(startPaused ? "pause=yes,start=" : "pause=no,start=") + std::to_string(std::max(0.0, startPosition));
        if (!run({"loadfile", file.c_str(), "replace", "-1", options.c_str()})) return false;
        if(logPlayback)debuglog::write("play.submit","serial="+std::to_string(serial+1)+" ms="+std::to_string(debuglog::elapsed(loadStarted)));
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
        const auto stream = "ffmpeg://"+decoderFile(current);
        const auto options = std::string(pauseRequested ? "pause=yes" : "pause=no")+
            ",stream-lavf-o=seekable=0,demuxer-seekable-cache=yes,demuxer-max-bytes=1048576";
        if (!run({"loadfile",stream.c_str(),"replace","-1",options.c_str()})) return false;
        if (get_property(handle,"playlist/0/id",MPV_FORMAT_INT64,&desiredEntry) < 0) return false;
        loaded = false; loadedSerial = 0; active = true; waitingStart = true;
        queuedSeek.reset(); seekPreview.reset(); info = {}; position = duration = 0;
        positionTime = Clock::now(); error.clear(); return true;
    }
    void setPaused(bool value) {
        if(nativeMode){paused=pauseRequested=value;if(native&&!nativeEditing)native->pause(value);return;}
        if(worker){position=displayPosition();positionTime=Clock::now();paused=pauseRequested=value;if(!value)seekPreview.reset();enqueue(1,[value](Audio& core){core.setPaused(value);});return;}
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
        if(nativeMode){
            if(!native||native->state().rate<=0)return;
            const auto target=std::llround(static_cast<long double>(*queuedSeek)*native->state().rate);
            pendingSeek=++seekSequence;queuedSeek.reset();native->seek(target);return;
        }
        const auto value = std::to_string(*queuedSeek);
        const char* args[] = {"seek",value.c_str(),"absolute+exact",nullptr};
        pendingSeek = ++seekSequence; queuedSeek.reset();
        if(worker){const auto target=std::stod(value);const auto sequence=pendingSeek;enqueue(4,[target,sequence](Audio& core){core.seekSequence=sequence-1;core.pendingSeek=0;core.queuedSeek=core.seekPreview=target;core.submitSeek();});return;}
        const auto result = command_async(handle,pendingSeek,args);
        if (result < 0) { error = error_string(result); pendingSeek = 0; seekPreview.reset(); }
    }
    void setVolume(double value) {
        if(nativeMode){if(!std::isfinite(value))return;volume=std::clamp(value,0.,100.);if(native&&!nativeEditing)native->gain(static_cast<float>(volume/100*std::pow(10.,gain/20)));return;}
        if(worker){volume=value;enqueue(2,[value](Audio& core){core.setVolume(value);});return;}
        int result = set_property(handle, "volume", MPV_FORMAT_DOUBLE, &value);
        if (result < 0) error = error_string(result); else volume = value;
    }
    void setGain(double value) {
        if (!std::isfinite(value)) return;
        value = std::clamp(value, -96.0, 12.0);
        if(nativeMode){gain=value;if(native&&!nativeEditing)native->gain(static_cast<float>(volume/100*std::pow(10.,gain/20)));return;}
        if(worker){gain=value;enqueue(3,[value](Audio& core){core.setGain(value);});return;}
        const auto result = set_property(handle, "volume-gain", MPV_FORMAT_DOUBLE, &value);
        if (result < 0) error = error_string(result); else gain = value;
    }
    std::string textProperty(const char* name) const {
        if(nativeMode)throw std::logic_error("libmpv properties are unavailable on native playback.");
        char* value = nullptr;
        if (get_property(handle, name, MPV_FORMAT_STRING, &value) < 0) return {};
        const std::unique_ptr<char, decltype(free)> owner(value, free);
        return value ? value : "";
    }
    std::vector<std::pair<std::string, std::string>> loadedTags() const {
        if(nativeMode||worker)return cachedTags;
        std::vector<std::pair<std::string, std::string>> result;
        if (!handle || !loaded || !free_node_contents) return result;
        mpv_node metadata{};
        if (get_property(handle, "metadata", MPV_FORMAT_NODE, &metadata) < 0) return result;
        struct NodeOwner {
            const Audio* audio; mpv_node& value;
            ~NodeOwner() { audio->free_node_contents(&value); }
        } owner{this, metadata};
        return tagsFromNode(metadata);
    }
    // Property-change event nodes are borrowed until the next wait_event.
    // Copy their strings without freeing libmpv's event-owned storage.
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
        info.sampleFormat = textProperty("audio-params/format");
        info.channelLayout = textProperty("audio-params/channels");
        get_property(handle, "audio-params/samplerate", MPV_FORMAT_INT64, &info.sampleRate);
        get_property(handle, "audio-params/channel-count", MPV_FORMAT_INT64, &info.channels);
        get_property(handle, "current-tracks/audio/demux-bitrate", MPV_FORMAT_DOUBLE, &info.bitrate);
    }
    bool poll() {
        if(nativeMode)return pollNative();
        if(worker)return collectWorker();
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
                if(logPlayback)debuglog::write("play.loaded","serial="+std::to_string(serial)+" ms="+std::to_string(debuglog::elapsed(loadStarted)));
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
        updateMeter();
        return eof;
    }
};

#include "audio_worker.h"

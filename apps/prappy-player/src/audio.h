#pragma once
#include "paths.h"
#include "debug_log.h"
#include "output_meter.h"
#include "native_playback.h"
#include <vector>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <chrono>
#include <memory>
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
// Native source-clock playback; the shared session owns the only editing PCM.
struct Audio {
    Audio()=default;Audio(const Audio&)=delete;Audio& operator=(const Audio&)=delete;
    bool nativeMode=false,nativeEditing=false,loaded=false,paused=false,active=false,pauseRequested=false;
    sampling::Frame nativeReturnFrame=0;
    std::optional<sampling::PcmBudget> nativeMemory;
    std::shared_ptr<sampling::NativePlayback> native;
    sampling::Connection nativeConnection;
    std::shared_ptr<const prepared::CatalogSource> preparedSource;
    std::vector<std::pair<std::string,std::string>> cachedTags;
    std::string error;fs::path current;AudioInfo info;
    uint64_t serial=0,loadedSerial=0,seekSequence=0,pendingSeek=0;
    double position=0,duration=0,volume=70,gain=0,meterPeak=0;
    std::unique_ptr<OutputMeter> outputMeter;bool meterAvailable=false,logPlayback=true;
    using Clock=std::chrono::steady_clock;Clock::time_point positionTime=Clock::now(),loadStarted=Clock::now();
    std::optional<double> queuedSeek,seekPreview;
    std::function<void()> notify;
    double displayPosition(Clock::time_point = Clock::now())const{return seekPreview.value_or(position);}
    void setNotify(std::function<void()> callback){notify=std::move(callback);}
    void openNative(bool silent,sampling::PcmBudget memory){
        if(nativeMode)throw std::logic_error("Audio is already open.");
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
            if(descriptor.contains("analysis"))source->analysis(descriptor.at("analysis"));
            info.codec=source->codec;info.sampleRate=state.rate;info.channels=source->native.channels;info.sampleFormat=descriptor.at("sample_format");preparedSource=std::move(source);
            if(logPlayback)debuglog::write("play.loaded","serial="+std::to_string(serial)+" ms="+std::to_string(debuglog::elapsed(loadStarted))+" native_rate="+std::to_string(state.rate));
        }
        if(pendingSeek&&!state.buffering){pendingSeek=0;seekPreview.reset();submitSeek();}
        updateMeter();return eof;
    }
    void pause() { setPaused(!paused); }
    void seek(double seconds) {
        if (!loaded || !std::isfinite(seconds)) return;
        const auto target = std::max(0.0,seconds);
        queuedSeek = seekPreview = duration > 0 ? std::min(target,duration) : target;
        submitSeek();
    }
    bool poll(){return pollNative();}
    void setPaused(bool value){paused=pauseRequested=value;if(native&&!nativeEditing)native->pause(value);}
    void submitSeek(){
        if(pendingSeek||!queuedSeek||!native||native->state().rate<=0)return;
        const auto target=std::llround(static_cast<long double>(*queuedSeek)*native->state().rate);
        pendingSeek=++seekSequence;queuedSeek.reset();native->seek(target);
    }
    void setVolume(double value){if(!std::isfinite(value))return;volume=std::clamp(value,0.,100.);applyGain();}
    void setGain(double value){if(!std::isfinite(value))return;gain=std::clamp(value,-96.,12.);applyGain();}
    void applyGain(){if(native&&!nativeEditing)native->gain(static_cast<float>(volume/100*std::pow(10.,gain/20)));}
    auto loadedTags()const{return cachedTags;}
    bool run(std::initializer_list<const char*> args){
        if(args.size()!=1||std::string_view(*args.begin())!="stop")throw std::logic_error("Unsupported native playback command.");
        if(native)native->stop();loaded=active=false;current.clear();preparedSource.reset();queuedSeek.reset();seekPreview.reset();pendingSeek=0;return true;
    }
};

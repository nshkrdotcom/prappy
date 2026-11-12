#pragma once
#include "sample_transport.h"
#include "native_session.h"
#include <vector>
#include <SDL3/SDL.h>
#include <memory>
#include <stdexcept>
namespace sampling {
// The callback swaps preallocated immutable blocks; the UI thread reclaims them.
class Audition {
    using Buffer=NativeSession::Buffer;
    PcmBudget memory;
    PcmBudget::Permit overhead;
    std::vector<float> scratch=std::vector<float>(8192);
    Frame nativeRate=Rate,nativeChannels=2;
    prepared::DeviceLayout layout{2,{}};
    SDL_AudioStream* stream=nullptr;
    std::shared_ptr<const Buffer> current,next,retired;
    Frame stride()const{return layout.channels*4;}
    static Frame end(const Buffer& block){return block.pcm->start()+block.pcm->frames();}
    static bool contains(const Buffer& block,Frame frame){return frame>=block.pcm->start()&&frame<end(block);}
    Span range;
    Frame initial=0,feeding=0,submitted=0,deviceFrames=0;
    bool repeat=false,paused=false,done=false,starved=false,audioInitialized=false;
    Uint64 lastFeed=0;
    Frame at(Frame consumed)const{
        const auto offset=initial-range.start+std::max<Frame>(0,consumed),length=range.end-range.start;
        return repeat?range.start+offset%length:std::min(range.end,range.start+offset);
    }
    Frame consumed()const{return std::max<Frame>(0,submitted-std::max(0,SDL_GetAudioStreamQueued(stream))/stride()-deviceFrames);}
    static void SDLCALL provide(void* user,SDL_AudioStream* target,int additional,int){
        auto& self=*static_cast<Audition*>(user);if(!self.paused)self.fill(target,additional);
    }
    void fill(SDL_AudioStream* target,int additional){
        auto& self=*this;if(!self.current)return;
        const auto bytes=self.stride();
        int remaining=static_cast<int>(additional/bytes*bytes);
        while(remaining>0){
            if(self.feeding>=self.range.end){
                if(!self.repeat){if(!self.done)SDL_FlushAudioStream(target);self.done=true;break;}
                self.feeding=self.range.start;
            }
            if(!contains(*self.current,self.feeding)){
                if(self.next&&contains(*self.next,self.feeding)){
                    self.current.swap(self.next);self.retired.swap(self.next);
                }else{self.starved=true;break;}
            }
            self.starved=false;
            const auto frames=std::min({static_cast<Frame>(remaining/bytes),static_cast<Frame>((self.scratch.size()*sizeof(float))/bytes),end(*self.current)-self.feeding,self.range.end-self.feeding});
            if(frames<=0)break;
            const auto index=self.feeding-self.current->pcm->start();
            for(Frame frame=0;frame<frames;++frame){
                auto* output=self.scratch.data()+static_cast<size_t>(frame)*self.layout.channels;
                std::fill_n(output,self.layout.channels,0.f);
                for(Frame channel=0;channel<self.nativeChannels;++channel)
                    output[self.layout.inputs[static_cast<size_t>(channel)]]=static_cast<float>(self.current->pcm->sample(index+frame,channel));
            }
            if(!SDL_PutAudioStreamData(target,self.scratch.data(),static_cast<int>(frames*bytes)))break;
            self.feeding+=frames;self.submitted+=frames;remaining-=static_cast<int>(frames*bytes);self.lastFeed=SDL_GetTicksNS();
        }
    }
public:
    struct Snapshot {Frame cursor=0,needed=-1,submitted=0;bool finished=false,buffering=false;};
    ~Audition(){if(stream)SDL_DestroyAudioStream(stream);if(audioInitialized)SDL_QuitSubSystem(SDL_INIT_AUDIO);}
    Audition():Audition(PcmBudget{}){}
    explicit Audition(PcmBudget budget):memory(std::move(budget)),overhead(memory.reserve(8*1024*1024)){}
    Audition(const Audition&)=delete;Audition& operator=(const Audition&)=delete;
    void start(std::shared_ptr<const Buffer> block,Span span,Frame cursor,bool loop,bool startPaused=false,std::vector<std::string> speakers={}){
        if(!block||span.end<=span.start||cursor<span.start||cursor>=span.end||!contains(*block,cursor))
            throw std::runtime_error("Invalid native audition timeline or PCM buffer.");
        const prepared::DeviceLayout nextLayout(block->pcm->source().channels,std::move(speakers));
        if(stream)SDL_PauseAudioStreamDevice(stream);
        layout=nextLayout;
        nativeRate=block->pcm->source().rate;nativeChannels=block->pcm->source().channels;
        const SDL_AudioSpec spec{SDL_AUDIO_F32,layout.channels,static_cast<int>(nativeRate)};
        if(!stream){
            if(!audioInitialized){if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw std::runtime_error(SDL_GetError());audioInitialized=true;}
            stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,provide,this);
            if(!stream)throw std::runtime_error(SDL_GetError());
        }
        SDL_AudioSpec output{};int frames=0;
        if(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream),&output,&frames)&&output.freq>0)deviceFrames=static_cast<Frame>(std::ceil(static_cast<double>(frames)*nativeRate/output.freq));
        std::shared_ptr<const Buffer> previous,previousNext,previousRetired;
        SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);SDL_ClearAudioStream(stream);
        if(!SDL_SetAudioStreamFormat(stream,&spec,nullptr)){
            SDL_UnlockAudioStream(stream);throw std::runtime_error(SDL_GetError());
        }
        // Never free potentially multi-megabyte native blocks while the SDL
        // callback is locked. Reclaim old owners on the UI thread afterwards.
        previous=std::move(current);previousNext=std::move(next);previousRetired=std::move(retired);
        current=std::move(block);range=span;initial=feeding=cursor;submitted=0;repeat=loop;paused=startPaused;done=starved=false;lastFeed=SDL_GetTicksNS();
        fill(stream,static_cast<int>((scratch.size()*sizeof(float))));
        SDL_UnlockAudioStream(stream);if(!paused)SDL_ResumeAudioStreamDevice(stream);
    }
    SDL_AudioDeviceID device()const{return stream?SDL_GetAudioStreamDevice(stream):0;}
    void enqueue(std::shared_ptr<const Buffer> block){
        if(!block||block->pcm->source().rate!=nativeRate||block->pcm->source().channels!=nativeChannels)
            throw std::runtime_error("Native audition buffer format changed.");
        std::shared_ptr<const Buffer> old,previous;
        SDL_LockAudioStream(stream);old=std::move(next);previous=std::move(retired);next=std::move(block);SDL_UnlockAudioStream(stream);
    }
    Snapshot snapshot(){
        Snapshot result;std::shared_ptr<const Buffer> previous;if(!stream)return result;
        SDL_LockAudioStream(stream);
        if(!current){SDL_UnlockAudioStream(stream);return result;}
        if(!repeat&&feeding>=range.end&&!done){SDL_FlushAudioStream(stream);done=true;}
        result.submitted=submitted;
        const auto queued=SDL_GetAudioStreamQueued(stream);
        const bool drained=!paused&&queued<=0&&SDL_GetTicksNS()-lastFeed>=static_cast<Uint64>(deviceFrames)*1000000000ULL/nativeRate;
        result.cursor=done&&drained?range.end:at(drained?submitted:consumed());result.finished=done&&drained;result.buffering=starved&&queued<=0;
        if(retired&&!contains(*retired,result.cursor))previous=std::move(retired);
        if(!next&&!done){
            const auto wanted=feeding>=range.end&&repeat?range.start:feeding;
            if(wanted<range.end&&!contains(*current,wanted))result.needed=wanted;
            else if(end(*current)-feeding<nativeRate*5){
                const auto following=std::min(end(*current),range.end);
                if(following<range.end)result.needed=following;
                else if(repeat&&!contains(*current,range.start))result.needed=range.start;
            }
        }
        SDL_UnlockAudioStream(stream);return result;
    }
    void pause(bool value){if(!stream)return;if(value)SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);paused=value;SDL_UnlockAudioStream(stream);if(!value)SDL_ResumeAudioStreamDevice(stream);}
    void loop(bool value){
        if(!stream)return;
        SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);
        if(!current){SDL_UnlockAudioStream(stream);return;}
        const auto cursor=at(consumed());
        if(!contains(*current,cursor)){
            if(retired&&contains(*retired,cursor))current.swap(retired);
            else if(next&&contains(*next,cursor))current.swap(next);
        }
        std::shared_ptr<const Buffer> oldNext;
        SDL_ClearAudioStream(stream);repeat=value;initial=feeding=std::min(cursor,range.end-1);submitted=0;done=starved=false;oldNext=std::move(next);lastFeed=SDL_GetTicksNS();
        fill(stream,static_cast<int>((scratch.size()*sizeof(float))));
        SDL_UnlockAudioStream(stream);if(!paused)SDL_ResumeAudioStreamDevice(stream);
    }
    void stop(){
        if(!stream)return;
        SDL_PauseAudioStreamDevice(stream);
        std::shared_ptr<const Buffer> oldCurrent,oldNext,oldRetired;
        SDL_LockAudioStream(stream);SDL_ClearAudioStream(stream);
        oldCurrent=std::move(current);oldNext=std::move(next);oldRetired=std::move(retired);
        paused=false;SDL_UnlockAudioStream(stream);
    }
    void gain(float value){if(stream)SDL_SetAudioStreamGain(stream,value);}
};
}

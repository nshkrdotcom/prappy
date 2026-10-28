#pragma once
#include "sample_transport.h"
#include <SDL3/SDL.h>
#include <memory>
#include <stdexcept>
namespace sampling {
// The callback swaps preallocated immutable blocks; the UI thread reclaims them.
class Audition {
    struct Block {
        std::vector<float> pcm;Frame origin;
        bool contains(Frame f)const{return f>=origin&&f<end();}
        Frame end()const{return origin+static_cast<Frame>(pcm.size()/2);}
    };
    SDL_AudioStream* stream=nullptr;
    std::unique_ptr<Block> current,next,retired;
    Span range;
    Frame initial=0,feeding=0,submitted=0,deviceFrames=0;
    bool repeat=false,paused=false,done=false,starved=false,audioInitialized=false;
    Uint64 lastFeed=0;
    Frame at(Frame consumed)const{
        const auto offset=initial-range.start+std::max<Frame>(0,consumed),length=range.end-range.start;
        return repeat?range.start+offset%length:std::min(range.end,range.start+offset);
    }
    Frame consumed()const{return std::max<Frame>(0,submitted-std::max(0,SDL_GetAudioStreamQueued(stream))/8-deviceFrames);}
    static void SDLCALL provide(void* user,SDL_AudioStream* target,int additional,int){
        auto& self=*static_cast<Audition*>(user);if(self.paused||!self.current)return;
        int remaining=std::min(additional,32768)&~7;
        while(remaining>0){
            if(self.feeding>=self.range.end){
                if(!self.repeat){if(!self.done)SDL_FlushAudioStream(target);self.done=true;break;}
                self.feeding=self.range.start;
            }
            if(!self.current->contains(self.feeding)){
                if(self.next&&self.next->contains(self.feeding)){
                    self.current.swap(self.next);self.retired.swap(self.next);
                }else{self.starved=true;break;}
            }
            self.starved=false;
            const auto frames=std::min({static_cast<Frame>(remaining/8),self.current->end()-self.feeding,self.range.end-self.feeding});
            if(frames<=0)break;
            const auto index=static_cast<size_t>(self.feeding-self.current->origin)*2;
            if(!SDL_PutAudioStreamData(target,self.current->pcm.data()+index,static_cast<int>(frames*8)))break;
            self.feeding+=frames;self.submitted+=frames;remaining-=static_cast<int>(frames*8);self.lastFeed=SDL_GetTicksNS();
        }
    }
public:
    struct Snapshot {Frame cursor=0,needed=-1;bool finished=false,buffering=false;};
    ~Audition(){if(stream)SDL_DestroyAudioStream(stream);if(audioInitialized)SDL_QuitSubSystem(SDL_INIT_AUDIO);}
    Audition()=default;Audition(const Audition&)=delete;Audition& operator=(const Audition&)=delete;
    void start(std::vector<float> pcm,Frame origin,Span span,Frame cursor,bool loop,bool startPaused=false){
        auto block=std::make_unique<Block>(Block{std::move(pcm),origin});
        if(span.end<=span.start||cursor<span.start||cursor>=span.end||block->pcm.empty()||block->pcm.size()%2||!block->contains(cursor))throw std::runtime_error("Invalid audition timeline or PCM block.");
        if(!stream){
            if(!audioInitialized){if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw std::runtime_error(SDL_GetError());audioInitialized=true;}
            const SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};
            stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,provide,this);
            if(!stream)throw std::runtime_error(SDL_GetError());
            SDL_AudioSpec output{};int frames=0;
            if(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream),&output,&frames)&&output.freq>0)deviceFrames=static_cast<Frame>(std::ceil(static_cast<double>(frames)*Rate/output.freq));
        }
        SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);SDL_ClearAudioStream(stream);
        current=std::move(block);next.reset();retired.reset();range=span;initial=feeding=cursor;submitted=0;repeat=loop;paused=startPaused;done=starved=false;lastFeed=SDL_GetTicksNS();
        SDL_UnlockAudioStream(stream);if(!paused)SDL_ResumeAudioStreamDevice(stream);
    }
    void enqueue(std::vector<float> pcm,Frame origin){
        if(pcm.empty()||pcm.size()%2)throw std::runtime_error("Invalid prefetched stereo PCM block.");
        auto block=std::make_unique<Block>(Block{std::move(pcm),origin});std::unique_ptr<Block> old,previous;
        SDL_LockAudioStream(stream);old=std::move(next);previous=std::move(retired);next=std::move(block);SDL_UnlockAudioStream(stream);
    }
    Snapshot snapshot(){
        Snapshot result;if(!stream)return result;
        SDL_LockAudioStream(stream);
        if(!current){SDL_UnlockAudioStream(stream);return result;}
        if(!repeat&&feeding>=range.end&&!done){SDL_FlushAudioStream(stream);done=true;}
        const auto queued=SDL_GetAudioStreamQueued(stream);
        const bool drained=!paused&&queued<=0&&SDL_GetTicksNS()-lastFeed>=static_cast<Uint64>(deviceFrames)*1000000000ULL/Rate;
        result.cursor=done&&drained?range.end:at(drained?submitted:consumed());result.finished=done&&drained;result.buffering=starved&&queued<=0;
        if(!next&&!done){
            const auto wanted=feeding>=range.end&&repeat?range.start:feeding;
            if(wanted<range.end&&!current->contains(wanted))result.needed=wanted;
            else if(current->end()-feeding<Rate*5){
                const auto following=std::min(current->end(),range.end);
                if(following<range.end)result.needed=following;
                else if(repeat&&!current->contains(range.start))result.needed=range.start;
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
        if(!current->contains(cursor)){
            if(retired&&retired->contains(cursor))current.swap(retired);
            else if(next&&next->contains(cursor))current.swap(next);
        }
        SDL_ClearAudioStream(stream);repeat=value;initial=feeding=std::min(cursor,range.end-1);submitted=0;done=starved=false;next.reset();lastFeed=SDL_GetTicksNS();
        SDL_UnlockAudioStream(stream);if(!paused)SDL_ResumeAudioStreamDevice(stream);
    }
    void stop(){if(stream){SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);SDL_ClearAudioStream(stream);current.reset();next.reset();retired.reset();paused=false;SDL_UnlockAudioStream(stream);}}
    void gain(float value){if(stream)SDL_SetAudioStreamGain(stream,value);}
};
}

#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>
namespace sampling {
class Audition {
    SDL_AudioStream* stream=nullptr;
    std::vector<float> samples;
    size_t position=0;
    bool repeat=false,audioInitialized=false;
    static void SDLCALL provide(void* user,SDL_AudioStream* target,int additional,int) {
        auto& self=*static_cast<Audition*>(user);
        int remaining=std::min(additional,32768) & ~7;
        while(remaining>0 && !self.samples.empty()){
            if(self.position==self.samples.size()){
                if(!self.repeat)break;
                self.position=0;
            }
            const auto count=std::min(self.samples.size()-self.position,static_cast<size_t>(remaining)/sizeof(float));
            if(!count)break;
            if(!SDL_PutAudioStreamData(target,self.samples.data()+self.position,static_cast<int>(count*sizeof(float))))break;
            self.position+=count;remaining-=static_cast<int>(count*sizeof(float));
        }
    }
public:
    ~Audition(){if(stream)SDL_DestroyAudioStream(stream);if(audioInitialized)SDL_QuitSubSystem(SDL_INIT_AUDIO);}
    Audition()=default;Audition(const Audition&)=delete;Audition& operator=(const Audition&)=delete;
    void play(std::vector<float> pcm,bool loop) {
        if(!stream){
            if(!audioInitialized){if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw std::runtime_error(SDL_GetError());audioInitialized=true;}
            const SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};
            stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,provide,this);
            if(!stream)throw std::runtime_error(SDL_GetError());
        }
        SDL_LockAudioStream(stream);SDL_ClearAudioStream(stream);samples=std::move(pcm);position=0;repeat=loop;SDL_UnlockAudioStream(stream);
        SDL_ResumeAudioStreamDevice(stream);
    }
    void stop(){if(stream){SDL_PauseAudioStreamDevice(stream);SDL_LockAudioStream(stream);samples.clear();position=0;SDL_ClearAudioStream(stream);SDL_UnlockAudioStream(stream);}}
    void gain(float value){if(stream)SDL_SetAudioStreamGain(stream,value);}
};
}

#pragma once
#include "prepared_audio.h"
#include <algorithm>
#include <memory>
#include <mutex>

namespace sampling {
class PcmBudget {
    struct State {std::mutex mutex;size_t used=0,limit;explicit State(size_t n):limit(n){}};
    std::shared_ptr<State> state;
    static size_t bytes(size_t mib){
        if(mib<64||mib>1024)throw std::runtime_error("Editor PCM budget must be 64 to 1024 MiB.");
        return mib*1024*1024;
    }
public:
    struct Reservation {
        std::shared_ptr<State> owner;size_t bytes;
        Reservation(std::shared_ptr<State> state,size_t count):owner(std::move(state)),bytes(count){}
        Reservation(const Reservation&)=delete;Reservation& operator=(const Reservation&)=delete;
        ~Reservation(){std::lock_guard lock(owner->mutex);owner->used-=bytes;}
    };
    using Permit=std::shared_ptr<const Reservation>;
    explicit PcmBudget(size_t mib=256):state(std::make_shared<State>(bytes(mib))){}
    size_t used()const{std::lock_guard lock(state->mutex);return state->used;}
    size_t limit()const{std::lock_guard lock(state->mutex);return state->limit;}
    void resize(size_t mib){
        const auto capacity=bytes(mib);std::lock_guard lock(state->mutex);
        if(state->used>capacity)throw std::runtime_error("Release retained PCM before reducing its budget.");
        state->limit=capacity;
    }
    Permit reserve(size_t count){
        std::lock_guard lock(state->mutex);
        if(!count||count>state->limit-state->used)throw std::runtime_error("Editor PCM budget is full.");
        // Construct before charging: allocation failure cannot leak a reservation.
        auto permit=std::make_shared<Reservation>(state,count);state->used+=count;return permit;
    }
    bool owns(const Permit& permit)const{return permit&&permit->owner==state;}
};

class NativeSession {
public:
    struct Buffer {std::shared_ptr<const prepared::Block> pcm;PcmBudget::Permit memory;};
private:
    prepared::NativeTimeline source;
    std::string sourceKey,timeline;
    uint64_t generation;
    PcmBudget budget;
    std::vector<std::shared_ptr<const Buffer>> buffers;
public:
    NativeSession(prepared::NativeTimeline native,std::string key,std::string id,uint64_t epoch,PcmBudget memory)
        :source(native),sourceKey(std::move(key)),timeline(std::move(id)),generation(epoch),budget(std::move(memory)){
        source.validate();
    }
    std::shared_ptr<const Buffer> adopt(std::shared_ptr<const prepared::Block> pcm,PcmBudget::Permit memory,
                                      std::string_view key,std::string_view id,uint64_t epoch){
        if(epoch!=generation)return {};
        if(key!=sourceKey||id!=timeline||!pcm||!budget.owns(memory)||memory->bytes!=pcm->bytes().size()||
           pcm->bytes().size()>32ULL*1024*1024||pcm->source().rate!=source.rate||pcm->source().channels!=source.channels||
           pcm->source().type!=source.type||pcm->source().frames!=source.frames)
            throw std::runtime_error("Native session response identity, memory or clock differs.");
        auto buffer=std::make_shared<const Buffer>(Buffer{std::move(pcm),std::move(memory)});
        buffers.push_back(buffer);
        std::sort(buffers.begin(),buffers.end(),[](const auto& a,const auto& b){return a->pcm->start()<b->pcm->start();});
        return buffer;
    }
    std::shared_ptr<const Buffer> find(prepared::Frame frame)const{
        const auto found=std::find_if(buffers.rbegin(),buffers.rend(),[&](const auto& b){
            return frame>=b->pcm->start()&&frame-b->pcm->start()<b->pcm->frames();});
        return found==buffers.rend()?nullptr:*found;
    }
    void retain(prepared::Frame start,prepared::Frame end){
        if(start<0||end<start||end>source.frames)throw std::runtime_error("Invalid native session window.");
        std::erase_if(buffers,[&](const auto& b){return b->pcm->start()>=end||b->pcm->start()+b->pcm->frames()<=start;});
    }
};
}

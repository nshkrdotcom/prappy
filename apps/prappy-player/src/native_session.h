#pragma once
#include "prepared_audio.h"
#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <stop_token>

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
        const std::shared_ptr<State> owner;const size_t bytes;
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
    Permit reserve(size_t count)const{
        std::lock_guard lock(state->mutex);
        if(!count||count>state->limit-state->used)throw std::runtime_error("Editor PCM budget is full.");
        // Construct before charging: allocation failure cannot leak a reservation.
        auto permit=std::make_shared<Reservation>(state,count);state->used+=count;return permit;
    }
    bool owns(const Permit& permit)const{return permit&&permit->owner==state;}
};

// Stored audio is immutable; readers supply their already-budgeted destination.
class PcmBacking {
public:
    struct Stored {virtual ~Stored()=default;virtual size_t bytes()const=0;virtual void read(std::span<std::byte>)const=0;};
    virtual ~PcmBacking()=default;
    virtual std::shared_ptr<const Stored> store(std::span<const std::byte>)=0;
    virtual std::shared_ptr<PcmBacking> fork()const=0;
};

class NativeSession {
public:
    struct Buffer {std::shared_ptr<const prepared::Block> pcm;PcmBudget::Permit memory;std::string decoderEpoch;};
    struct Clip {
        struct Segment {std::shared_ptr<const Buffer> buffer;std::shared_ptr<const PcmBacking::Stored> stored;prepared::Frame first,count,origin;std::string decoderEpoch;};
        prepared::NativeTimeline source;prepared::Frame first,last;
        std::string sourceKey,timeline;mutable std::string decoderEpoch;
        using Loader=std::function<std::shared_ptr<const Buffer>(prepared::Frame,prepared::Frame,std::stop_token)>;
        mutable std::vector<Segment> segments;mutable std::vector<std::string> decoderEpochs;PcmBudget budget;
        Loader load;std::shared_ptr<PcmBacking> backing;mutable std::mutex mutex;
        void remember(const std::string& epoch)const{
            if(epoch.empty())throw std::runtime_error("Selected audio lacks decoder provenance.");
            if(std::find(decoderEpochs.begin(),decoderEpochs.end(),epoch)==decoderEpochs.end())decoderEpochs.push_back(epoch);
            if(decoderEpochs.size()>512)throw std::runtime_error("Selection has too many decoder restarts to save safely.");
            decoderEpoch=decoderEpochs.size()==1?decoderEpochs.front():"mixed";
        }
        uint64_t bytes()const{
            source.validate();const auto stride=static_cast<uint64_t>(source.channels)*prepared::sampleBytes(source.type);
            if(first<0||last<=first||last>source.frames||static_cast<uint64_t>(last-first)>UINT64_MAX/stride)
                throw std::runtime_error("Native clip byte count is invalid.");
            return static_cast<uint64_t>(last-first)*stride;
        }
        template<class Consumer> void each(Consumer consume,std::stop_token stop={})const{
            std::lock_guard lock(mutex);
            source.validate();
            const auto stride=static_cast<size_t>(source.channels)*prepared::sampleBytes(source.type);
            // Each pass writes its lazy spans to a separate backing transaction.
            // A failed/cancelled pass must not pin unreferenced compressed bytes
            // in the file used by successful earlier passes.
            std::shared_ptr<PcmBacking> writing;
            const auto emit=[&](std::span<const std::byte> bytes){for(size_t at=0;at<bytes.size();at+=65536){
                if(stop.stop_requested())throw std::runtime_error("Native selection save cancelled.");
                consume(bytes.subspan(at,std::min<size_t>(65536,bytes.size()-at)));
            }};
            for(auto& part:segments){
                if(stop.stop_requested())throw std::runtime_error("Native selection save cancelled.");
                if(!part.buffer&&!part.stored){
                    if(!load||!backing)throw std::runtime_error("Save reader has no backing or PCM loader.");
                    auto block=load(part.first,part.count,stop);
                    if(!block||!block->pcm||block->pcm->start()!=part.first||block->pcm->frames()!=part.count||
                       block->pcm->source().rate!=source.rate||block->pcm->source().channels!=source.channels||
                       block->pcm->source().type!=source.type||block->pcm->source().frames!=source.frames||
                       !budget.owns(block->memory)||block->memory->bytes!=block->pcm->bytes().size())
                        throw std::runtime_error("Save reader returned a different native span.");
                    if(block->decoderEpoch.empty()||
                       (decoderEpochs.size()>=512&&std::find(decoderEpochs.begin(),decoderEpochs.end(),block->decoderEpoch)==decoderEpochs.end()))
                        throw std::runtime_error("Selected audio lacks supported decoder provenance.");
                    if(!writing)writing=backing->fork();
                    auto stored=writing->store(block->pcm->bytes());
                    remember(block->decoderEpoch);part.decoderEpoch=block->decoderEpoch;
                    part.stored=std::move(stored);
                    emit(block->pcm->bytes());continue;
                }
                if(part.origin<0||part.first<part.origin||part.count<=0||part.first>source.frames||part.count>source.frames-part.first||
                   static_cast<uint64_t>(part.first-part.origin)>SIZE_MAX/stride||
                   static_cast<uint64_t>(part.count)>SIZE_MAX/stride)
                    throw std::runtime_error("Stored native selection span is invalid.");
                const auto offset=static_cast<size_t>(part.first-part.origin)*stride;
                const auto size=static_cast<size_t>(part.count)*stride;
                const auto available=part.stored?part.stored->bytes():part.buffer->pcm->bytes().size();
                if(offset>available||size>available-offset||
                   (part.buffer&&(part.buffer->pcm->start()!=part.origin||
                       part.buffer->pcm->source().rate!=source.rate||part.buffer->pcm->source().channels!=source.channels||
                       part.buffer->pcm->source().type!=source.type||part.buffer->pcm->source().frames!=source.frames)))
                    throw std::runtime_error("Stored native selection bytes differ from the source.");
                PcmBudget::Permit scratch;std::vector<std::byte> raw;
                if(part.stored){scratch=budget.reserve(part.stored->bytes());raw.resize(part.stored->bytes());part.stored->read(raw);}
                const auto* data=(part.stored?raw.data():part.buffer->pcm->bytes().data())+offset;
                emit(std::span<const std::byte>(data,size));
            }
        }
    };

private:
    prepared::NativeTimeline source;
    std::string sourceKey,timeline,decoderEpoch;
    uint64_t generation;
    PcmBudget budget;
    mutable std::vector<std::shared_ptr<const Buffer>> buffers;
    std::vector<Clip::Segment> cold;
    std::shared_ptr<PcmBacking> backing;
    std::optional<std::pair<prepared::Frame,prepared::Frame>> preserved;
    std::vector<Clip::Segment> pieces()const{
        auto result=cold;
        for(const auto& b:buffers)result.push_back({b,{},b->pcm->start(),b->pcm->frames(),b->pcm->start(),b->decoderEpoch});
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.first<b.first;});return result;
    }
public:
    NativeSession(prepared::NativeTimeline native,std::string key,std::string id,uint64_t epoch,PcmBudget memory)
        :source(native),sourceKey(std::move(key)),timeline(std::move(id)),generation(epoch),budget(std::move(memory)){
        source.validate();
    }
    bool preserving()const{return preserved.has_value();}
    bool hasBacking()const{return static_cast<bool>(backing);}
    void preserve(prepared::Frame start,prepared::Frame end,std::shared_ptr<PcmBacking> storage={}){
        if(start<0||end<=start||end>source.frames||(!backing&&!storage))throw std::runtime_error("Invalid preserved selection.");
        if(preserved){start=std::min(start,preserved->first);end=std::max(end,preserved->second);}
        if(!backing)backing=std::move(storage);
        preserved={{start,end}};
    }
    void clearPreservation(){preserved.reset();}
    void restart(uint64_t epoch){if(epoch<=generation)throw std::runtime_error("Native session generation did not advance.");generation=epoch;decoderEpoch.clear();}
    prepared::Frame nextMissingEnd(prepared::Frame start,prepared::Frame end)const{
        for(const auto& piece:cold){if(start>=piece.first&&start<piece.first+piece.count)return start;if(piece.first>start)end=std::min(end,piece.first);}
        for(const auto& b:buffers){if(start>=b->pcm->start()&&start<b->pcm->start()+b->pcm->frames())return start;if(b->pcm->start()>start)end=std::min(end,b->pcm->start());}
        return end;
    }
    std::shared_ptr<const Buffer> adopt(std::shared_ptr<const prepared::Block> pcm,PcmBudget::Permit memory,
                                      std::string_view key,std::string_view id,uint64_t epoch,std::string_view decoder={}){
        if(epoch!=generation)return {};
        if(key!=sourceKey||id!=timeline||!pcm||!budget.owns(memory)||memory->bytes!=pcm->bytes().size()||
           pcm->bytes().size()>32ULL*1024*1024||pcm->source().rate!=source.rate||pcm->source().channels!=source.channels||
           pcm->source().type!=source.type||pcm->source().frames!=source.frames)
            throw std::runtime_error("Native session response identity, memory or clock differs.");
        if(!preserved&&!decoderEpoch.empty()&&decoder!=decoderEpoch)throw std::runtime_error("Decoder session changed; reopen the bounded region before extending its audio.");
        if(preserved&&nextMissingEnd(pcm->start(),pcm->start()+pcm->frames())!=pcm->start()+pcm->frames())
            throw std::runtime_error("Decoder output would replace authoritative selection audio.");
        for(const auto& existing:buffers)
            if(pcm->start()<existing->pcm->start()+existing->pcm->frames()&&
               existing->pcm->start()<pcm->start()+pcm->frames())
                throw std::runtime_error("Native decoder returned overlapping resident audio.");
        std::string nextEpoch(decoder);
        auto buffer=std::make_shared<const Buffer>(Buffer{std::move(pcm),std::move(memory),nextEpoch});
        buffers.push_back(buffer);
        std::sort(buffers.begin(),buffers.end(),[](const auto& a,const auto& b){return a->pcm->start()<b->pcm->start();});
        decoderEpoch.swap(nextEpoch);
        return buffer;
    }
    std::shared_ptr<const Buffer> find(prepared::Frame frame)const{
        const auto found=std::find_if(buffers.rbegin(),buffers.rend(),[&](const auto& b){
            return frame>=b->pcm->start()&&frame-b->pcm->start()<b->pcm->frames();});
        if(found!=buffers.rend())return *found;
        const auto saved=std::find_if(cold.begin(),cold.end(),[&](const auto& p){return frame>=p.first&&frame<p.first+p.count;});
        if(saved==cold.end())return {};
        const auto stride=static_cast<size_t>(source.channels)*prepared::sampleBytes(source.type);
        if(saved->count<=0||static_cast<uint64_t>(saved->count)>32ULL*1024*1024/stride||
           saved->stored->bytes()!=static_cast<size_t>(saved->count)*stride)
            throw std::runtime_error("Stored PCM size differs from the native source clock.");
        auto permit=budget.reserve(saved->stored->bytes());auto temporary=budget.reserve(saved->stored->bytes());
        std::vector<std::byte> raw(saved->stored->bytes());saved->stored->read(raw);
        auto pcm=std::make_shared<prepared::Block>(source,saved->first,saved->count,raw);
        auto buffer=std::make_shared<const Buffer>(Buffer{std::move(pcm),std::move(permit),saved->decoderEpoch});
        buffers.push_back(buffer);std::sort(buffers.begin(),buffers.end(),[](const auto& a,const auto& b){return a->pcm->start()<b->pcm->start();});return buffer;
    }
    bool covers(prepared::Frame start,prepared::Frame end)const{
        if(start<0||end<=start||end>source.frames)return false;
        auto cursor=start;
        for(const auto& piece:pieces()){
            if(piece.first>cursor)return false;
            cursor=std::max(cursor,piece.first+piece.count);
            if(cursor>=end)return true;
        }
        return false;
    }
    prepared::Frame first()const{return buffers.empty()?0:buffers.front()->pcm->start();}
    prepared::Frame last()const{
        prepared::Frame end=0;for(const auto& buffer:buffers)end=std::max(end,buffer->pcm->start()+buffer->pcm->frames());return end;
    }
    prepared::Frame frames(prepared::Frame first,prepared::Frame last)const{
        if(first<0||last<first||last>source.frames)throw std::runtime_error("Invalid native cache interval.");
        prepared::Frame count=0,end=first;
        for(const auto& buffer:buffers){
            const auto stop=std::min(last,buffer->pcm->start()+buffer->pcm->frames());
            count+=std::max<prepared::Frame>(0,stop-std::max(end,buffer->pcm->start()));end=std::max(end,stop);
        }
        return count;
    }
    prepared::Frame frames()const{return frames(0,source.frames);}
    std::shared_ptr<const Clip> clip(prepared::Frame start,prepared::Frame end,Clip::Loader load={})const{
        if(start<0||end<=start||end>source.frames||(!covers(start,end)&&(!load||!backing)))
            throw std::runtime_error("Save requires the complete selected audio window.");
        auto result=std::make_shared<Clip>();result->source=source;result->first=start;result->last=end;result->budget=budget;
        result->sourceKey=sourceKey;result->timeline=timeline;result->load=std::move(load);
        auto at=start;const auto stride=static_cast<size_t>(source.channels)*prepared::sampleBytes(source.type);
        const auto gap=[&](prepared::Frame stop){
            if(!result->backing)result->backing=backing->fork();
            while(at<stop){
            const auto count=std::min<prepared::Frame>(stop-at,1024*1024/stride);
            result->segments.push_back({{},{},at,count,at,{}});at+=count;
        }};
        for(auto piece:pieces()){
            if(piece.first>=end)break;
            if(piece.first+piece.count<=at)continue;
            if(piece.first>at)gap(piece.first);
            const auto stop=std::min(end,piece.first+piece.count);piece.first=at;piece.count=stop-at;
            result->remember(piece.decoderEpoch);result->segments.push_back(std::move(piece));at=stop;if(at==end)break;
        }
        if(at<end)gap(end);
        if(result->decoderEpoch.empty())result->decoderEpoch="pending";
        return result;
    }
    void retain(prepared::Frame start,prepared::Frame end){
        if(start<0||end<start||end>source.frames)throw std::runtime_error("Invalid native session window.");
        std::vector<Clip::Segment> staged;
        const auto stride=static_cast<size_t>(source.channels)*prepared::sampleBytes(source.type);
        std::shared_ptr<PcmBacking> writing;
        if(preserved)for(const auto& b:buffers){
            if(b->pcm->start()<end&&b->pcm->start()+b->pcm->frames()>start)continue;
            auto at=std::max(b->pcm->start(),preserved->first);const auto stop=std::min(b->pcm->start()+b->pcm->frames(),preserved->second);
            while(at<stop){
                const auto existing=std::find_if(cold.begin(),cold.end(),[&](const auto& p){return at>=p.first&&at<p.first+p.count;});
                if(existing!=cold.end()){at=std::min(stop,existing->first+existing->count);continue;}
                const auto newPiece=std::find_if(staged.begin(),staged.end(),[&](const auto& p){return at>=p.first&&at<p.first+p.count;});
                if(newPiece!=staged.end()){at=std::min(stop,newPiece->first+newPiece->count);continue;}
                const auto count=std::min<prepared::Frame>(stop-at,1024*1024/stride);
                if(!writing)writing=backing->fork();
                auto stored=writing->store(std::span(b->pcm->bytes()).subspan(static_cast<size_t>(at-b->pcm->start())*stride,static_cast<size_t>(count)*stride));
                staged.push_back({{},std::move(stored),at,count,at,b->decoderEpoch});at+=count;
            }
        }
        // Reserve both commits before mutating state: disk/quota failure or a
        // vector allocation cannot evict the authoritative resident buffer.
        cold.reserve(cold.size()+staged.size());
        if(!staged.empty())backing=backing->fork();
        cold.insert(cold.end(),std::make_move_iterator(staged.begin()),std::make_move_iterator(staged.end()));
        std::erase_if(buffers,[&](const auto& b){return b->pcm->start()>=end||b->pcm->start()+b->pcm->frames()<=start;});
    }
};
}

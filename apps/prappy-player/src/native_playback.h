#pragma once
#include "sample_client.h"
#include "sample_audition.h"
#include "prepared_requests.h"
#include <cmath>

namespace sampling {
// UI-thread controller. The shared session owns PCM; the callback only reads it.
class NativePlayback {
public:
    struct State {Frame cursor=0,rate=0,frames=0,submitted=0;uint64_t transfers=0;bool loaded=false,paused=true,buffering=false,finished=false;std::string error;};
private:
    PcmBudget budget;
    Audition output;
    Client client;
    std::shared_ptr<NativeSession> retained;
    Json manifest;
    State status;
    std::string endpoint,capture,original,owner,leasePath;
    Span coverage{},range{};
    std::optional<Span> cacheWindow;
    Frame decodedEnd=-1,pendingStart=0,pendingEnd=0;
    uint64_t generation=0;
    bool pending=false,started=false,repeat=false;
    float level=1;
    double initialSeconds=0;
    std::chrono::steady_clock::time_point renewDue{};
    void release(){
        if(!leasePath.empty())client.submit({"source-session-close","DELETE",leasePath,nullptr,generation,{}});
        leasePath.clear();
    }
    prepared::NativeTimeline timeline()const{
        prepared::NativeTimeline t{prepared::integer(manifest.at("sample_rate")),prepared::integer(manifest.at("channels")),
            prepared::integer(manifest.at("frame_count")),prepared::sampleType(manifest.at("sample_format").get<std::string>())};
        t.validate();return t;
    }
    void newOwner(){
        client.cancelReads(ReadScope::PCM);release();output.stop();pending=started=false;decodedEnd=-1;++generation;owner=uuid();
        retained=std::make_shared<NativeSession>(timeline(),manifest.at("source_key"),manifest.at("timeline_id"),generation,budget);
        if(manifest.at("access_mode")=="indexed")leasePath=endpoint+"/session?source_key="+manifest.at("source_key").get<std::string>()+
            "&timeline="+manifest.at("timeline_id").get<std::string>()+"&owner="+owner;
        renewDue=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    }
    void feed(std::shared_ptr<const NativeSession::Buffer> block){
        if(!started){
            if(status.cursor<block->pcm->start()||status.cursor>=block->pcm->start()+block->pcm->frames())
                throw std::runtime_error("Native cursor "+std::to_string(status.cursor)+" outside received span "+std::to_string(block->pcm->start())+".."+std::to_string(block->pcm->start()+block->pcm->frames()));
            output.start(std::move(block),range,status.cursor,repeat,status.paused);output.gain(level);started=true;}
        else output.enqueue(std::move(block));
        status.loaded=true;status.buffering=false;
    }
    bool keepWindow()const{
        if(!cacheWindow||!retained)return false;
        const auto t=timeline();const size_t stride=static_cast<size_t>(t.channels)*prepared::sampleBytes(t.type);
        const auto resident=static_cast<size_t>(retained->frames())*stride;
        const auto working=budget.limit()-budget.used()+resident;
        const auto extra=resident-static_cast<size_t>(retained->frames(cacheWindow->start,cacheWindow->end))*stride;
        const auto frames=cacheWindow->end-cacheWindow->start;
        if(static_cast<uint64_t>(frames)>(working-extra)/stride)return false;
        const auto bytes=static_cast<size_t>(frames)*stride;
        return bytes+extra+std::min<size_t>(32*1024*1024,bytes)<=working;
    }
    void request(Frame frame,bool forPlayback=true){
        const auto bound=forPlayback?range:cacheWindow.value_or(coverage);
        if(frame<bound.start||frame>=bound.end)return;
        if(auto cached=retained->find(frame)){if(forPlayback)feed(std::move(cached));return;}
        if(pending)return;
        const auto t=timeline();
        const size_t stride=static_cast<size_t>(t.channels)*prepared::sampleBytes(t.type);
        if(keepWindow())retained->retain(cacheWindow->start,cacheWindow->end);
        else{
            const auto working=budget.limit()-budget.used()+static_cast<size_t>(retained->frames())*stride;
            const Frame history=std::min(t.rate*5,static_cast<Frame>(working/4/stride));
            retained->retain(std::max<Frame>(0,status.cursor-history),status.cursor+std::min(t.frames-status.cursor,t.rate*30));
        }
        const auto available=std::min<size_t>(32*1024*1024,(budget.limit()-budget.used())/2);
        const Frame count=std::min({bound.end-frame,t.rate*(forPlayback?3:30),static_cast<Frame>(available/stride)});
        if(count<=0){if(forPlayback)status.buffering=true;return;}
        auto context=prepared::pcmContext(manifest,frame,count);context["pcm_epoch"]=generation;context["native_feed"]=forPlayback;
        Request message{"source-pcm","GET",endpoint+"/pcm?source_key="+manifest.at("source_key").get<std::string>()+
            "&timeline="+manifest.at("timeline_id").get<std::string>()+"&session="+owner+"&start="+std::to_string(frame)+"&count="+std::to_string(count),context,generation,{}};
        const auto bytes=static_cast<size_t>(count)*stride;
        message.incomingMemory=budget.reserve(bytes);message.pcmMemory=budget.reserve(bytes);
        pending=true;pendingStart=frame;pendingEnd=frame+count;client.submit(std::move(message));
    }
    void fillWindow(){
        if(pending||!keepWindow())return;
        Frame first=cacheWindow->start;
        while(first<cacheWindow->end){auto block=retained->find(first);if(!block)break;first=std::min(cacheWindow->end,block->pcm->start()+block->pcm->frames());}
        // Never seek a live decoder behind retained PCM during background fill.
        if(first<cacheWindow->end&&first==decodedEnd)request(first,false);
    }
    void descriptor(Json value){
        if(value.at("capture").at("id")!=capture||value.at("capture").at("blob_sha256")!=original)
            throw std::runtime_error("Native playback original identity changed.");
        const auto key=value.at("source_key").get<std::string>(),id=value.at("timeline_id").get<std::string>();
        if(key.size()!=64||key.find_first_not_of("0123456789abcdef")!=std::string::npos||!prepared::SourceRef::valid(id,"tl_"))
            throw std::runtime_error("Native playback source clock identity is invalid.");
        const auto mode=value.at("access_mode").get<std::string>();
        if(mode!="indexed"&&mode!="frozen")throw std::runtime_error("Unsupported native playback source.");
        manifest=std::move(value);const auto t=timeline();status.rate=t.rate;status.frames=t.frames;
        const auto span=manifest.value("coverage",Json{{"start_frame",0},{"end_frame",t.frames}});
        coverage={prepared::integer(span.at("start_frame")),prepared::integer(span.at("end_frame"))};
        if(coverage.start<0||coverage.end>t.frames||coverage.end<=coverage.start)throw std::runtime_error("Invalid native playback coverage.");
        range=coverage;
        const auto frame=std::round(static_cast<long double>(initialSeconds)*t.rate);
        status.cursor=frame>=coverage.end?coverage.end-1:std::max(coverage.start,static_cast<Frame>(frame));
        newOwner();request(status.cursor);
    }
public:
    NativePlayback(Connection connection,PcmBudget memory,std::function<void()> notify,Client::Transport transport={})
        :budget(std::move(memory)),output(budget),client(std::move(connection),std::move(notify),std::move(transport)){}
    ~NativePlayback(){release();}
    NativePlayback(const NativePlayback&)=delete;NativePlayback& operator=(const NativePlayback&)=delete;
    const State& state()const{return status;}
    const Json& descriptor()const{return manifest;}
    const std::string& reference()const{return endpoint;}
    std::shared_ptr<NativeSession> session()const{return retained;}
    void load(std::string path,std::string expectedCapture,std::string expectedOriginal,bool paused,double seconds,Json known=nullptr){
        if(!std::isfinite(seconds)||seconds<0||!prepared::SourceRef::valid(expectedCapture,"cap_")||
            expectedOriginal.size()!=64||expectedOriginal.find_first_not_of("0123456789abcdef")!=std::string::npos)
            throw std::runtime_error("Invalid native playback request.");
        stop();endpoint=std::move(path);capture=std::move(expectedCapture);original=std::move(expectedOriginal);initialSeconds=seconds;
        status.paused=paused;status.buffering=true;
        if(known.is_null())client.submit({"source-descriptor","GET",endpoint,nullptr,generation,{}});
        else descriptor(std::move(known));
    }
    void start(Span span,Frame frame,bool loop,bool paused){
        if(manifest.is_null()||span.start<coverage.start||span.end>coverage.end||span.end<=span.start||frame<span.start||frame>=span.end)
            throw std::runtime_error("Playback range exceeds verified native coverage.");
        output.stop();started=false;range=span;status.cursor=frame;status.paused=paused;status.finished=false;status.buffering=true;repeat=loop;status.error.clear();
        if(!retained->find(frame)&&(!pending||frame<pendingStart||frame>=pendingEnd))newOwner();request(frame);
    }
    void clearCache(){cacheWindow.reset();}
    void loop(bool value){repeat=value;output.loop(value);}
    void cache(Span window){
        if(manifest.is_null()||window.start<coverage.start||window.end>coverage.end||window.end<=window.start)
            throw std::runtime_error("Cache window exceeds verified native coverage.");
        cacheWindow=window;
    }
    void seek(Frame frame){if(manifest.is_null())throw std::runtime_error("Native source clock is not loaded.");start(range,std::clamp(frame,range.start,range.end-1),repeat,status.paused);}
    void pause(bool value){status.paused=value;if(started)output.pause(value);}
    void gain(float value){if(!std::isfinite(value)||value<0)throw std::runtime_error("Invalid playback gain.");level=value;output.gain(value);}
    void stop(){
        client.cancelReads(ReadScope::Audio);release();output.stop();retained.reset();manifest=nullptr;
        ++generation;pending=started=repeat=false;cacheWindow.reset();decodedEnd=-1;status=State{};
    }
    void tick(){
        try{
            for(auto& reply:client.collect()){
                if(reply.request.generation!=generation)continue;
                if(!reply.error.empty())throw std::runtime_error(reply.error);
                if(reply.request.tag=="source-descriptor")descriptor(std::move(reply.body));
                else if(reply.request.tag=="source-pcm"){
                    pending=false;if(!retained)continue;
                    auto block=retained->adopt(reply.preparedPcm,reply.request.pcmMemory,reply.request.body.at("source_key"),
                        reply.request.body.at("timeline_id"),generation,reply.body.at("decoder_epoch").get<std::string>());
                    if(block){++status.transfers;decodedEnd=block->pcm->start()+block->pcm->frames();if(reply.request.body.value("native_feed",true))feed(std::move(block));}
                }else if(reply.request.tag=="source-session"&&!reply.body.value("active",false))
                    throw std::runtime_error("Native playback decoder expired; retained audio is unchanged.");
            }
            if(manifest.is_null()||!status.error.empty())return;
            if(started){
                const auto state=output.snapshot();status.submitted=state.submitted;status.cursor=state.cursor;status.finished=state.finished;status.buffering=state.buffering;
                if(!state.finished&&state.needed>=0)request(state.needed);
            }else request(status.cursor);
            fillWindow();
            const auto now=std::chrono::steady_clock::now();
            if(!leasePath.empty()&&now>=renewDue){renewDue=now+std::chrono::seconds(15);client.submit({"source-session","GET",leasePath,nullptr,generation,{}});}
        }catch(const std::exception& e){status.error=e.what();status.buffering=false;pending=false;output.stop();}
    }
};
}

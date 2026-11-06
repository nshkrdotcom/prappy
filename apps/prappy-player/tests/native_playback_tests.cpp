#include "native_playback.h"
#include <iostream>
#include <atomic>
#include <cstring>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Predicate> static void until(sampling::NativePlayback& player,Predicate done){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do{player.tick();if(done())return;if(!player.state().error.empty())throw std::runtime_error(player.state().error);SDL_Delay(1);}while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("Native transport timed out at frame "+std::to_string(player.state().cursor));
}
int main(){
 try{
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
    using namespace sampling;using namespace prepared;
    const std::string capture="cap_"+std::string(32,'a'),sha(64,'b'),key(64,'c'),timeline="tl_"+std::string(32,'d');
    for(const auto rate:{44100,48000})for(const auto kind:{SampleType::Float32,SampleType::Int32,SampleType::Float64}){
        const std::string format=kind==SampleType::Float64?"float64":kind==SampleType::Int32?"int32":"float32";
        const Frame total=rate==48000&&kind==SampleType::Float64?5000000000LL:rate*8;
        Json descriptor={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",2},{"sample_format",format},{"access_mode","indexed"},
            {"capture",{{"id",capture},{"blob_sha256",sha}}}};
        auto reads=std::make_shared<std::atomic<int>>(0),selectedReads=std::make_shared<std::atomic<int>>(0);
        Client::Transport transport=[descriptor,rate,kind,total,reads,selectedReads](const Connection&,const Request& request,std::stop_token){
            Reply reply;reply.status=200;
            if(request.tag=="source-descriptor")reply.body=descriptor;
            else if(request.tag=="source-pcm"){
                ++*reads;const Frame start=request.body.at("origin_frame"),count=request.body.at("frame_count");
                if(start==rate*6+17)++*selectedReads;
                std::vector<std::byte> bytes(static_cast<size_t>(count)*2*sampleBytes(kind));
                for(size_t i=0;i<static_cast<size_t>(count)*2;++i){
                    if(kind==SampleType::Float64){const double v=.125+1e-11;std::memcpy(bytes.data()+i*8,&v,8);}
                    else if(kind==SampleType::Int32){const int32_t v=268435457;std::memcpy(bytes.data()+i*4,&v,4);}
                    else{const float v=.125f;std::memcpy(bytes.data()+i*4,&v,4);}
                }
                reply.preparedPcm=std::make_shared<Block>(NativeTimeline{rate,2,total,kind},start,count,std::move(bytes));reply.body={{"decoder_epoch","same-owner"}};
            }else reply.body={{"active",true}};
            return reply;
        };
        PcmBudget memory(64);Connection connection;connection.token=std::string(48,'t');
        {
            NativePlayback player(connection,memory,[]{},transport);
            player.load("/source/audio",capture,sha,true,1.25);
            until(player,[&]{return player.state().loaded;});
            const Frame origin=rate*5/4;
            require(player.state().paused&&player.state().cursor==origin,"Paused source frame shifted");
            require(player.session()->first()==origin&&player.session()->find(origin),"First native span differs");
            require(*reads>=1&&memory.used()<=memory.limit(),"Native transport bypassed its shared budget");
            player.seek(rate*6+17);
            until(player,[&]{return player.session()&&player.session()->find(rate*6+17);});
            require(player.state().cursor==rate*6+17&&player.state().paused,"Seek frame or pause state changed");
            const auto beforeCached=selectedReads->load();
            player.start({rate*6+17,rate*6+101},rate*6+17,false,false);
            until(player,[&]{return player.state().finished;});
            require(player.state().cursor==rate*6+101,"Native EOF did not stop at the exact frame");
            require(selectedReads->load()==beforeCached,"Cached range playback decoded the source again");
            if(total>uint64_t{1}<<32){
                player.start({total-17,total},total-17,false,true);
                until(player,[&]{return player.session()&&player.session()->find(total-17);});
                require(player.state().cursor==total-17&&player.state().frames==total,"64-bit native playback coordinate was truncated");
            }
            player.stop();
        }
        const auto released=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(memory.used()&&std::chrono::steady_clock::now()<released)SDL_Delay(1);
        require(memory.used()==0,"Transport leaked shared PCM reservations");
        {
            auto occupied=memory.reserve(56ULL*1024*1024);
            NativePlayback player(connection,memory,[]{},transport);
            const auto before=reads->load();player.load("/source/audio",capture,sha,true,0);
            until(player,[&]{return !player.descriptor().is_null();});
            require(!player.state().loaded&&player.state().buffering&&reads->load()==before,"Budget pressure bypassed backpressure");
            occupied.reset();until(player,[&]{return player.state().loaded;});
            require(memory.used()<=memory.limit(),"Retried read exceeded its shared budget");
        }
        {
            const Json frozen={ {"start_frame",100},{"end_frame",183} };
            Client::Transport fixed=[transport,frozen](const Connection& c,const Request& request,std::stop_token stop){
                auto reply=transport(c,request,stop);
                if(request.tag=="source-descriptor"){reply.body["access_mode"]="frozen";reply.body["coverage"]=frozen;}
                return reply;
            };
            NativePlayback player(connection,memory,[]{},fixed);player.load("/region/audio",capture,sha,true,0);
            until(player,[&]{return player.state().loaded;});
            require(player.state().cursor==100&&player.session()->first()==100&&player.session()->last()==183,"Frozen coverage was not enforced");
            bool rejected=false;try{player.start({99,183},100,false,true);}catch(const std::exception&){rejected=true;}
            require(rejected,"Frozen playback expanded into unpreserved source audio");
        }
        {
            auto blocked=std::make_shared<std::atomic<bool>>(false);
            Client::Transport delayed=[transport,blocked](const Connection& c,const Request& request,std::stop_token stop){
                if(request.tag=="source-pcm"&&request.body.at("origin_frame")==0){
                    *blocked=true;while(!stop.stop_requested())SDL_Delay(1);
                    Reply reply;reply.error="cancelled";return reply;
                }
                return transport(c,request,stop);
            };
            NativePlayback player(connection,memory,[]{},delayed);player.load("/source/audio",capture,sha,true,0);
            until(player,[&]{return blocked->load();});player.seek(rate*6+17);
            until(player,[&]{return player.state().loaded;});
            require(player.session()->first()==rate*6+17&&player.state().cursor==rate*6+17,"Superseded decoder response replaced the sought frame");
        }
        {
            NativePlayback player(connection,memory,[]{},transport);player.load("/source/audio",capture,std::string(64,'f'),true,0);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(player.state().error.empty()&&std::chrono::steady_clock::now()<deadline){player.tick();SDL_Delay(1);}
            require(!player.state().loaded&&!player.state().error.empty()&&!player.session(),"Wrong original identity entered playback");
        }
    }
    {
        SDL_SetHint(SDL_HINT_AUDIO_DUMMY_TIMESCALE,"0.1");SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,"2048");
        const Frame rate=192000,total=rate*15;const int channels=5;
        const Json description={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",channels},{"sample_format","float64"},{"access_mode","indexed"},
            {"capture",{{"id",capture},{"blob_sha256",sha}}}};
        Client::Transport transport=[=](const Connection&,const Request& request,std::stop_token){
            Reply reply;reply.status=200;
            if(request.tag=="source-descriptor")reply.body=description;
            else if(request.tag=="source-pcm"){
                const Frame start=request.body.at("origin_frame"),count=request.body.at("frame_count");
                reply.preparedPcm=std::make_shared<Block>(NativeTimeline{rate,channels,total,SampleType::Float64},start,count,
                    std::vector<std::byte>(static_cast<size_t>(count)*channels*8));reply.body={{"decoder_epoch","large-session"}};
            }else reply.body={{"active",true}};
            return reply;
        };
        PcmBudget memory(64);Connection connection;connection.token=std::string(48,'t');
        NativePlayback player(connection,memory,[]{},transport);player.load("/large/audio",capture,sha,false,0);
        until(player,[&]{require(memory.used()<=memory.limit(),"Sliding playback exceeded its cap");return player.state().cursor>=rate*9;});
    }
    std::cout<<"PASS native playback: exact 44.1/48 kHz frame, native types, pause/seek/EOF, cancellation, frozen bounds and shared backpressure\n";
    return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

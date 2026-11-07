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
static void speakerOutputTest(){
    using namespace sampling;using namespace prepared;
    struct Probe {
        std::array<std::atomic<float>,8> peak{};
        static void SDLCALL receive(void* context,const SDL_AudioSpec* spec,float* pcm,int bytes){
            auto& self=*static_cast<Probe*>(context);
            for(int i=0;i<bytes/4;++i){const auto channel=static_cast<size_t>(i%spec->channels);const float value=std::abs(pcm[i]);
                if(channel<self.peak.size()&&value>self.peak[channel].load())self.peak[channel]=value;}
        }
    };
    const std::vector<std::string> order{"FL","FR","FC","SL","SR"};
    for(size_t channel=0;channel<order.size();++channel){
        PcmBudget budget(64);const NativeTimeline source{192000,5,192000,SampleType::Float64};
        std::vector<double> samples(static_cast<size_t>(source.frames)*5);
        for(size_t at=channel;at<samples.size();at+=5)samples[at]=.125+1e-11;
        auto pcm=std::make_shared<const Block>(source,0,source.frames,std::as_bytes(std::span(samples)));
        auto buffer=std::make_shared<const NativeSession::Buffer>(NativeSession::Buffer{pcm,budget.reserve(pcm->bytes().size())});
        Audition output(budget);output.start(buffer,{0,source.frames},0,true,true,order);
        SDL_AudioSpec device{};int frames=0;require(SDL_GetAudioDeviceFormat(output.device(),&device,&frames),"No dummy device format");
        require(device.channels<=8,"Unexpected output channel count");
        const SDL_AudioSpec input{SDL_AUDIO_F32,8,192000},destination{SDL_AUDIO_F32,device.channels,device.freq};
        auto* reference=SDL_CreateAudioStream(&input,&destination);require(reference!=nullptr,"Cannot create speaker reference");
        std::vector<float> bed(4096*8);const DeviceLayout mapping(5,order);
        for(size_t at=mapping.inputs[channel];at<bed.size();at+=8)bed[at]=.125f;
        require(SDL_PutAudioStreamData(reference,bed.data(),static_cast<int>(bed.size()*4))&&SDL_FlushAudioStream(reference),"Speaker reference failed");
        std::vector<float> expected(static_cast<size_t>(SDL_GetAudioStreamAvailable(reference))/4);
        require(SDL_GetAudioStreamData(reference,expected.data(),static_cast<int>(expected.size()*4))>0,"Speaker reference is empty");
        SDL_DestroyAudioStream(reference);std::array<float,8> maxima{};
        for(size_t i=0;i<expected.size();++i)maxima[i%device.channels]=std::max(maxima[i%device.channels],std::abs(expected[i]));
        Probe probe;require(SDL_SetAudioPostmixCallback(output.device(),Probe::receive,&probe),"Cannot capture speaker output");
        output.pause(false);const auto deadline=SDL_GetTicks()+3000;
        const auto matches=[&]{for(int ch=0;ch<device.channels;++ch)if(std::abs(probe.peak[ch].load()-maxima[ch])>=.0001f)return false;return true;};
        while(!matches()&&SDL_GetTicks()<deadline)SDL_Delay(1);
        output.pause(true);SDL_SetAudioPostmixCallback(output.device(),nullptr,nullptr);
        for(int ch=0;ch<device.channels;++ch)if(std::abs(probe.peak[ch].load()-maxima[ch])>=.0001f)
            throw std::runtime_error("Native speaker "+order[channel]+" output "+std::to_string(ch)+" peak "+std::to_string(probe.peak[ch].load())+" differs from "+std::to_string(maxima[ch]));
        require(budget.used()<=budget.limit(),"Speaker audition exceeded its shared budget");
    }
}
int main(){
 try{
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
    using namespace sampling;using namespace prepared;
    const std::string capture="cap_"+std::string(32,'a'),sha(64,'b'),key(64,'c'),timeline="tl_"+std::string(32,'d');
    speakerOutputTest();
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
            const Frame cachedEnd=std::min(total,origin+rate*7);
            player.cache({origin,cachedEnd});
            until(player,[&]{return player.session()->covers(origin,cachedEnd);});
            require(player.state().cursor==origin&&player.state().paused,"Background region fill changed audition or cursor");
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
    for(const int mode:{0,1,2,3,4})for(const Frame origin:{Frame{0},Frame{((mode==1||mode==4)?48000:44100)*5/4}})for(const bool looping:{false,true}){
        const Frame rate=(mode==1||mode==4)?48000:44100;
        SDL_SetHint(SDL_HINT_AUDIO_DUMMY_TIMESCALE,"0.1");
        const int channels=mode==3?1:mode==4?5:2;
        const Frame total=rate*7+113;
        Json description={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",channels},{"sample_format","float32"},{"access_mode","indexed"},
            {"capture",{{"id",capture},{"blob_sha256",sha}}}};
        if(channels==5)description["channel_order"]={"FL","FR","FC","SL","SR"};
        const auto signal=[](Frame frame,int channel){return static_cast<float>(((static_cast<uint64_t>(frame)*1664525+1013904223+channel*71237)%65536)/131072.-.25);};
        Client::Transport transport=[=](const Connection&,const Request& request,std::stop_token){
            Reply reply;reply.status=200;
            if(request.tag=="source-descriptor")reply.body=description;
            else if(request.tag=="source-pcm"){
                const Frame first=request.body.at("origin_frame"),count=request.body.at("frame_count");std::vector<std::byte> bytes(static_cast<size_t>(count)*channels*4);
                for(Frame i=0;i<count;++i)for(int c=0;c<channels;++c){float value=signal(first+i,c);std::memcpy(bytes.data()+static_cast<size_t>(i*channels+c)*4,&value,4);}
                reply.preparedPcm=std::make_shared<Block>(NativeTimeline{rate,channels,total,SampleType::Float32},first,count,std::move(bytes));reply.body={{"decoder_epoch","digital"}};
            }else reply.body={{"active",true}};
            return reply;
        };
        struct Device {
            SDL_AudioDeviceID id=0;
            explicit Device(bool force){if(force){require(SDL_InitSubSystem(SDL_INIT_AUDIO),"Cannot initialize forced output domain");const SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};id=SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec);require(id!=0,"Cannot open forced output domain");}}
            ~Device(){if(id){SDL_CloseAudioDevice(id);SDL_QuitSubSystem(SDL_INIT_AUDIO);}}
        } forcedOutput(mode==2);
        struct Capture{
            std::vector<float> samples=std::vector<float>(48000*80);std::atomic<size_t> count{0};std::atomic<int> rate{0},channels{0};
            static void SDLCALL receive(void* context,const SDL_AudioSpec* spec,float* pcm,int bytes){
                auto& self=*static_cast<Capture*>(context);const auto first=self.count.load();const auto size=std::min(static_cast<size_t>(bytes)/4,self.samples.size()-first);
                std::memcpy(self.samples.data()+first,pcm,size*4);self.rate=spec->freq;self.channels=spec->channels;self.count.store(first+size);
            }
        } captureOutput;
        PcmBudget memory(64);Connection connection;connection.token=std::string(48,'t');
        NativePlayback player(connection,memory,[]{},transport);player.load("/digital/audio",capture,sha,true,static_cast<double>(origin)/rate);
        until(player,[&]{return player.state().loaded;});
        require(SDL_SetAudioPostmixCallback(player.device(),Capture::receive,&captureOutput),"Cannot install digital output capture");
        const Frame loopFrames=rate/7+13;
        if(looping){player.start({origin,origin+loopFrames},origin,true,true);player.loop(false);player.loop(true);}
        player.gain(.5f);player.pause(false);
        until(player,[&]{return looping?player.state().submitted>=rate*2:player.state().finished;});
        player.pause(true);
        require(SDL_SetAudioPostmixCallback(player.device(),nullptr,nullptr),"Cannot detach digital output capture");
        require(captureOutput.rate==(mode==2?48000:rate)&&captureOutput.channels>=1,"Digital test did not use the expected output domain");
        const auto count=captureOutput.count.load();size_t first=0;while(first<count&&captureOutput.samples[first]==0)++first;
        const Frame expectedFrames=looping?rate:total-origin;
        const int referenceChannels=channels==5?8:channels;
        std::vector<float> reference(static_cast<size_t>(expectedFrames)*referenceChannels);
        for(Frame i=0;i<expectedFrames;++i)for(int c=0;c<channels;++c){
            const int speaker=channels==5&&c>=3?c+3:c;
            reference[static_cast<size_t>(i)*referenceChannels+speaker]=signal(origin+(looping?i%loopFrames:i),c)*.5f;
        }
        if(captureOutput.rate!=rate||captureOutput.channels!=referenceChannels){
            const SDL_AudioSpec input{SDL_AUDIO_F32,referenceChannels,static_cast<int>(rate)},output{SDL_AUDIO_F32,captureOutput.channels,captureOutput.rate};
            auto* stream=SDL_CreateAudioStream(&input,&output);require(stream!=nullptr,"Cannot create continuous resampling reference");
            require(SDL_PutAudioStreamData(stream,reference.data(),static_cast<int>(reference.size()*4))&&SDL_FlushAudioStream(stream),"Cannot feed continuous resampling reference");
            reference.resize(static_cast<size_t>(SDL_GetAudioStreamAvailable(stream))/4);
            require(SDL_GetAudioStreamData(stream,reference.data(),static_cast<int>(reference.size()*4))==static_cast<int>(reference.size()*4),"Cannot read continuous resampling reference");SDL_DestroyAudioStream(stream);
        }
        if(looping)reference.resize(reference.size()-256);
        require(first+reference.size()<=count,"Digital output truncated the selected span");
        float maximum=0;for(size_t i=0;i<reference.size();++i)maximum=std::max(maximum,std::abs(captureOutput.samples[first+i]-reference[i]));
        std::cout<<"DIGITAL source_rate="<<rate<<" source_channels="<<channels<<" output_channels="<<captureOutput.channels<<" output_rate="<<captureOutput.rate<<" looping="<<looping<<" start_frame="<<origin<<" compared_samples="<<reference.size()<<" maximum_error="<<maximum<<"\n";
        require(maximum<=(mode==2?2e-6f:0.f),"SDL digital output shifted, duplicated, omitted or changed a native sample");

    }
    SDL_SetHint(SDL_HINT_AUDIO_DUMMY_TIMESCALE,"1");
    {
        const Frame rate=48000,total=rate*300;
        const Json description={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",2},{"sample_format","float32"},{"access_mode","indexed"},
            {"capture",{{"id",capture},{"blob_sha256",sha}}}};
        auto reads=std::make_shared<std::atomic<int>>(0);
        Client::Transport transport=[=](const Connection&,const Request& request,std::stop_token){
            Reply reply;reply.status=200;
            if(request.tag=="source-descriptor")reply.body=description;
            else if(request.tag=="source-pcm"){
                ++*reads;const Frame first=request.body.at("origin_frame"),count=request.body.at("frame_count");
                reply.preparedPcm=std::make_shared<Block>(NativeTimeline{rate,2,total,SampleType::Float32},first,count,std::vector<std::byte>(static_cast<size_t>(count)*8));
                reply.body={{"decoder_epoch","five-minute"}};
            }else reply.body={{"active",true}};
            return reply;
        };
        PcmBudget memory(256);Connection connection;connection.token=std::string(48,'t');
        NativePlayback player(connection,memory,[]{},transport);player.load("/track/audio",capture,sha,true,0);
        until(player,[&]{return player.state().loaded;});player.cache({0,total});
        until(player,[&]{require(memory.used()<=memory.limit(),"Five-minute cache exceeded client budget");return player.session()->covers(0,total);});
        const auto before=reads->load();const auto session=player.session();
        player.start({total-100,total},total-100,false,true);player.tick();
        require(player.session()==session&&reads->load()==before&&player.state().cursor==total-100,"Whole-track cached chop decoded or shifted the region");
        require(player.session()->frames()==total,"Five-minute session lost retained PCM");
    }
    {
        SDL_SetHint(SDL_HINT_AUDIO_DUMMY_TIMESCALE,"0.1");SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,"2048");
        const Frame rate=192000,total=rate*15;const int channels=5;
        const Json description={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",channels},{"channel_order",{"FL","FR","FC","SL","SR"}},{"sample_format","float64"},{"access_mode","indexed"},
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

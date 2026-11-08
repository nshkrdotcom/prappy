#include "native_playback.h"
#include "native_spill.h"
#include <iostream>
#include <atomic>
#include <cstring>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Predicate> static void until(sampling::NativePlayback& player,Predicate done){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do{player.tick();if(done())return;if(!player.state().error.empty())throw std::runtime_error(player.state().error);SDL_Delay(1);}while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("Native transport timed out at frame "+std::to_string(player.state().cursor));
}
static void uploadFrameTests(){
    using sampling::UploadFrame;
    const UploadFrame ordinary(64,4096);require(ordinary.total==4164&&ordinary.sendLength==4164,"Small upload framing changed");
    const UploadFrame overGiB(64,1073741825ULL);require(overGiB.total==1073741893ULL,"Upload retained the arbitrary 1 GiB limit");
    const UploadFrame belowDWORD(64,4294967227ULL);require(belowDWORD.total==4294967295ULL&&belowDWORD.sendLength==4294967295U,"DWORD boundary shortened upload");
    const UploadFrame aboveDWORD(64,4294967228ULL);require(aboveDWORD.total==4294967296ULL&&aboveDWORD.sendLength==0,"Large upload wrapped its Content-Length");
    sampling::NativeSession::Clip clip;clip.source={192000,32,INT64_MAX,prepared::SampleType::Float64};clip.first=0;clip.last=INT64_MAX;
    bool overflow=false;try{(void)clip.bytes();}catch(const std::exception&){overflow=true;}require(overflow,"Clip PCM byte multiplication overflowed");
    clip.source={48000,2,6000000000LL,prepared::SampleType::Float32};clip.first=0;clip.last=6000000000LL;
    require(clip.bytes()==48000000000ULL,"Clip length lost native 64-bit extent");
    for(const auto [header,audio]:std::array<std::pair<size_t,uint64_t>,3>{{{0,1},{65537,1},{64,UINT64_MAX}}}){
        bool failed=false;try{const UploadFrame invalid(header,audio);}catch(const std::exception&){failed=true;}
        require(failed,"Invalid metadata or overflowing upload length was accepted");
    }
}
static void spillTests(){
    using namespace sampling;
    PcmBudget budget(64);const auto before=budget.used();
    std::shared_ptr<const PcmBacking::Stored> saved;
    std::vector<std::byte> input(1024*1024);uint32_t value=713;
    for(auto& byte:input){value^=value<<13;value^=value>>17;value^=value<<5;byte=static_cast<std::byte>(value);}
    {
        CompressedSpill backing(budget);saved=backing.store(input);
        const auto charged=budget.used(),stored=CompressedSpill::quotaUsed();auto saveStore=backing.fork();
        auto other=saveStore->store(input);std::vector<std::byte> copy(input.size());other->read(copy);
        require(copy==input&&budget.used()==charged&&CompressedSpill::quotaUsed()>stored,"Independent save backing duplicated scratch or did not charge storage");
        other.reset();saveStore.reset();require(CompressedSpill::quotaUsed()==stored,"Discarded save backing retained its storage quota");
        require(budget.used()>before,"Spill scratch escaped the PCM budget");
        std::vector<std::byte> result(input.size());saved->read(result);require(result==input,"Compressed spill changed authoritative native bits");
        bool failed=false;try{saved->read(std::span(result).first(result.size()-1));}catch(const std::exception&){failed=true;}
        require(failed,"Spill accepted an incorrectly sized destination");
    }
    require(budget.used()>before,"Spill lifetime ended before the immutable snapshot");
    saved.reset();require(budget.used()==before,"Spill ownership leaked its memory reservation");
    CompressedSpill tiny(budget,1);bool failed=false;try{tiny.store(input);}catch(const std::exception&){failed=true;}
    require(failed,"Spill ignored its explicit storage ceiling");
}
static void selectionAuthorityTests(){
    using namespace sampling;using namespace prepared;
    PcmBudget budget(64);const NativeTimeline source{48000,1,1000000,SampleType::Int32};
    NativeSession session(source,"key","timeline",7,budget);
    session.preserve(0,786432,std::make_shared<CompressedSpill>(budget));
    std::vector<std::byte> expected(3*1024*1024);
    for(size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::byte>((i*73+i/11)&255);
    for(Frame start=0;start<524288;start+=262144){
        auto block=std::make_shared<Block>(source,start,262144,std::span(expected).subspan(static_cast<size_t>(start)*4,1024*1024));
        session.adopt(block,budget.reserve(block->bytes().size()),"key","timeline",7,"epoch-a");
    }
    session.preserve(524288,786432); // Undo must retain audio from the previous selection.
    session.retain(786432,900000);
    require(session.frames()==0,"Eviction still pinned active selection PCM");
    auto saved=session.clip(0,524288);std::vector<std::byte> output;
    saved->each([&](std::span<const std::byte> bytes){output.insert(output.end(),bytes.begin(),bytes.end());});
    require(output==std::vector<std::byte>(expected.begin(),expected.begin()+2*1024*1024),"Eviction changed already-adopted native bits");
    require(session.nextMissingEnd(524288,900000)==900000&&session.nextMissingEnd(0,900000)==0,"Preserved coverage boundaries shifted");
    require(session.find(300000)->pcm->bytes()==std::vector<std::byte>(expected.begin()+1024*1024,expected.begin()+2*1024*1024),"Audition reread lost authoritative samples");
    session.preserve(0,786432);session.restart(8);
    auto next=std::make_shared<Block>(source,524288,262144,std::span(expected).subspan(2*1024*1024));
    session.adopt(next,budget.reserve(next->bytes().size()),"key","timeline",8,"epoch-b");
    auto mixed=session.clip(0,786432);require(mixed->decoderEpochs.size()==2,"Save lost decoder-span provenance");output.clear();
    mixed->each([&](std::span<const std::byte> bytes){output.insert(output.end(),bytes.begin(),bytes.end());});
    require(output==expected,"A new decoder owner replaced previous authoritative audio");
    auto nextGap=std::make_shared<Block>(source,786432,1,std::vector<std::byte>(4));
    session.adopt(nextGap,budget.reserve(4),"key","timeline",8,"epoch-c");
    bool overwrote=false;try{session.adopt(nextGap,budget.reserve(4),"key","timeline",8,"epoch-d");}catch(const std::exception&){overwrote=true;}
    require(overwrote,"A changed decoder epoch replaced an authoritative span");

}
static void missingSelectionTests(){
    using namespace sampling;using namespace prepared;
    PcmBudget budget(64);NativeTimeline source{48000,1,768,SampleType::Int32};NativeSession session(source,"key","timeline",1,budget);
    session.preserve(0,768,std::make_shared<CompressedSpill>(budget));
    std::vector<std::byte> expected(768*4);for(size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::byte>((i*17)&255);
    auto first=std::make_shared<Block>(source,0,128,std::span(expected).first(512));session.adopt(first,budget.reserve(512),"key","timeline",1,"heard");
    int reads=0;auto clip=session.clip(0,768,[&](Frame start,Frame count,std::stop_token){
        ++reads;require(start==128&&count==640,"Save decoded over already-authoritative frames");
        auto pcm=std::make_shared<Block>(source,start,count,std::span(expected).subspan(512));
        return std::make_shared<const NativeSession::Buffer>(NativeSession::Buffer{pcm,budget.reserve(pcm->bytes().size()),"save-reader"});
    });
    require(reads==0,"Opening a save snapshot decoded the selection synchronously");
    std::vector<std::byte> output;clip->each([&](std::span<const std::byte> b){output.insert(output.end(),b.begin(),b.end());});
    require(output==expected&&reads==1&&clip->decoderEpochs.size()==2,"Bounded gap fill lost selection bits or provenance");
    output.clear();clip->each([&](std::span<const std::byte> b){output.insert(output.end(),b.begin(),b.end());});
    require(output==expected&&reads==1,"Upload re-decoded audio after hashing it");
    std::stop_source stop;stop.request_stop();bool cancelled=false;
    try{clip->each([](auto){},stop.get_token());}catch(const std::exception&){cancelled=true;}
    require(cancelled,"Snapshot streaming ignored cancellation");
}
static void failedEvictionTests(){
    using namespace sampling;using namespace prepared;
    struct Full: PcmBacking {std::shared_ptr<PcmBacking> fork()const override{return std::make_shared<Full>();}std::shared_ptr<const Stored> store(std::span<const std::byte>)override{throw std::runtime_error("injected full editing disk");}};
    PcmBudget budget(64);NativeTimeline source{48000,1,1000,SampleType::Int32};NativeSession session(source,"key","timeline",1,budget);
    session.preserve(0,1000,std::make_shared<Full>());auto pcm=std::make_shared<Block>(source,0,1000,std::vector<std::byte>(4000));
    session.adopt(pcm,budget.reserve(4000),"key","timeline",1,"heard");
    bool failed=false;try{session.retain(1000,1000);}catch(const std::exception&){failed=true;}
    require(failed&&session.covers(0,1000)&&session.find(0)->pcm==pcm&&budget.used()==4000,"Failed spill discarded authoritative audio or leaked PCM");
}
static void speakerOutputTest(){
    using namespace sampling;using namespace prepared;
    struct Probe {
        std::array<std::atomic<float>,8> peak{};std::atomic<uint64_t> frames{0};
        static void SDLCALL receive(void* context,const SDL_AudioSpec* spec,float* pcm,int bytes){
            auto& self=*static_cast<Probe*>(context);self.frames+=static_cast<uint64_t>(bytes/4/spec->channels);
            for(int i=0;i<bytes/4;++i){const auto channel=static_cast<size_t>(i%spec->channels);const float value=std::abs(pcm[i]);
                if(channel<self.peak.size()&&value>self.peak[channel].load())self.peak[channel]=value;}
        }
    };
    for(const auto& order:std::vector<std::vector<std::string>>{
            {"FL","FR","FC","SL","SR"},{"FL","FR","FC","LFE","BL","BR"},
            {"FL","FR","FC","LFE","BL","BR","SL","SR"}})
    for(size_t channel=0;channel<order.size();++channel){
        PcmBudget budget(64);const NativeTimeline source{192000,static_cast<int64_t>(order.size()),192000,SampleType::Float64};
        std::vector<double> samples(static_cast<size_t>(source.frames)*order.size());
        for(size_t at=channel;at<samples.size();at+=order.size())samples[at]=.125+1e-11;
        auto pcm=std::make_shared<const Block>(source,0,source.frames,std::as_bytes(std::span(samples)));
        auto buffer=std::make_shared<const NativeSession::Buffer>(NativeSession::Buffer{pcm,budget.reserve(pcm->bytes().size())});
        Audition output(budget);output.start(buffer,{0,source.frames},0,true,true,order);
        SDL_AudioSpec device{};int frames=0;require(SDL_GetAudioDeviceFormat(output.device(),&device,&frames),"No dummy device format");
        require(device.channels<=8,"Unexpected output channel count");
        const SDL_AudioSpec input{SDL_AUDIO_F32,8,192000},destination{SDL_AUDIO_F32,device.channels,device.freq};
        auto* reference=SDL_CreateAudioStream(&input,&destination);require(reference!=nullptr,"Cannot create speaker reference");
        std::vector<float> bed(4096*8);const DeviceLayout mapping(static_cast<int64_t>(order.size()),order);
        for(size_t at=mapping.inputs[channel];at<bed.size();at+=8)bed[at]=.125f;
        require(SDL_PutAudioStreamData(reference,bed.data(),static_cast<int>(bed.size()*4))&&SDL_FlushAudioStream(reference),"Speaker reference failed");
        std::vector<float> expected(static_cast<size_t>(SDL_GetAudioStreamAvailable(reference))/4);
        require(SDL_GetAudioStreamData(reference,expected.data(),static_cast<int>(expected.size()*4))>0,"Speaker reference is empty");
        SDL_DestroyAudioStream(reference);std::array<float,8> maxima{};
        for(size_t i=0;i<expected.size();++i)maxima[i%device.channels]=std::max(maxima[i%device.channels],std::abs(expected[i]));
        Probe probe;require(SDL_SetAudioPostmixCallback(output.device(),Probe::receive,&probe),"Cannot capture speaker output");
        output.pause(false);const auto deadline=SDL_GetTicks()+3000;
        const auto matches=[&]{if(!probe.frames.load())return false;for(int ch=0;ch<device.channels;++ch)if(std::abs(probe.peak[ch].load()-maxima[ch])>=.0001f)return false;return true;};
        while(!matches()&&SDL_GetTicks()<deadline)SDL_Delay(1);
        output.pause(true);SDL_SetAudioPostmixCallback(output.device(),nullptr,nullptr);
        require(probe.frames.load()>0,"Speaker probe did not receive device frames");
        for(int ch=0;ch<device.channels;++ch)if(std::abs(probe.peak[ch].load()-maxima[ch])>=.0001f)
            throw std::runtime_error("Native speaker "+order[channel]+" output "+std::to_string(ch)+" peak "+std::to_string(probe.peak[ch].load())+" differs from "+std::to_string(maxima[ch]));
        require(budget.used()<=budget.limit(),"Speaker audition exceeded its shared budget");
    }
}
int main(int argc,char** argv){
 try{
        uploadFrameTests();spillTests();selectionAuthorityTests();missingSelectionTests();failedEvictionTests();
        if(argc==2&&std::string_view(argv[1])=="--save-storage"){std::cout<<"PASS checked native save framing and bounded lossless storage\n";return 0;}
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
        NativePlayback player(connection,memory,[]{},transport);player.load("/large/audio",capture,sha,false,0);until(player,[&]{return player.state().loaded;});player.cache({0,total});
        until(player,[&]{require(memory.used()<=memory.limit(),"Sliding playback exceeded its cap");return player.state().cursor>=rate*9;});
        auto saved=player.saveClip(0,rate*9);uint64_t bytes=0;
        saved->each([&](std::span<const std::byte> data){require(std::all_of(data.begin(),data.end(),[](auto v){return v==std::byte{};}),"Wide double spill changed sample bits");bytes+=data.size();});
        require(bytes==static_cast<uint64_t>(rate)*9*channels*8&&bytes>memory.limit()&&memory.used()<=memory.limit(),"Wide double save did not remain bounded beyond RAM");
        std::cout<<"WIDE_SAVE native_bytes="<<bytes<<" pcm_reserved="<<memory.used()<<" pcm_limit="<<memory.limit()<<'\n';
    }
    std::cout<<"PASS native playback: 5.0/5.1/7.1 callback frames, exact clocks/types, pause/seek/EOF, cancellation and bounded authority; build "<<__DATE__<<" "<<__TIME__<<"\n";
    return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

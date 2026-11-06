#include "remote_catalog.h"
#include "remote_waveform.h"
#include "audio.h"
#include "prepared_requests.h"
#include <iostream>
#include <random>
#include <algorithm>

int main(int argc,char** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("Pass an isolated indexed fixture manifest and optional --measure.");
        const auto fixture=sampling::Json::parse(stateio::read(stateio::pathFromText(argv[1])));
        const auto directory=fs::temp_directory_path()/("prappy-indexed-client-"+sampling::uuid());
        fs::create_directory(directory);const auto config=directory/"player.ini";
        stateio::write(config,"debug_log=true\n");
        stateio::write(directory/"sampling-connection.json",sampling::Json{
            {"url",fixture.at("url")},{"token_file",fixture.at("token_file")}}.dump());
        Library library;ScanProgress progress;
        if(!refreshPreparedCatalog(library,config,{},progress))throw std::runtime_error(library.notice);
        if(library.tracks.size()!=fixture.at("expected_sources").get<size_t>()||!library.backend)
            throw std::runtime_error("Indexed catalog source count differs.");
        const auto track=std::find_if(library.tracks.begin(),library.tracks.end(),[&](const auto& t){return t.prepared&&t.prepared->source.capture==fixture.at("capture_id").get<std::string>();});
        if(track==library.tracks.end()||!track->prepared->ready())throw std::runtime_error("Fixture original is not playable.");
        const auto& source=*track->prepared;
        sampling::Request inspect;inspect.tag="source-descriptor";inspect.path=source.source.endpoint();
        const auto manifest=sampling::perform(*library.backend,inspect);
        if(!manifest.error.empty())throw std::runtime_error(manifest.error);
        const auto native=prepared::pcmContext(manifest.body,100,1000);
        const auto key=manifest.body.at("source_key").get<std::string>(),timeline=manifest.body.at("timeline_id").get<std::string>();
        auto read=[&](prepared::Frame origin,prepared::Frame count){
            sampling::Request request;request.tag="source-pcm";
            request.path=source.source.endpoint()+"/pcm?source_key="+key+"&timeline="+timeline+"&start="+std::to_string(origin)+"&count="+std::to_string(count);
            request.body=prepared::pcmContext(manifest.body,origin,count);
            auto reply=sampling::perform(*library.backend,request);
            if(!reply.error.empty()||!reply.preparedPcm||reply.preparedPcm->frames()!=count||reply.preparedPcm->start()!=origin||reply.body.at("decoder_epoch").get<std::string>().empty())
                throw std::runtime_error("Native PCM identity/count failed: "+reply.error);
            return reply;
        };
        read(100,1000);
        RemoteWaveform waveform([]{});waveform.select(source,*library.backend);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
        while(!waveform.value()&&std::chrono::steady_clock::now()<deadline){waveform.collect();Sleep(5);}
        if(!waveform.value()||waveform.value()->frames!=manifest.body.at("frame_count").get<uint64_t>()||waveform.value()->sampleRate!=manifest.body.at("sample_rate").get<uint64_t>()||waveform.value()->levels[0].empty())
            throw std::runtime_error("Native waveform failed: "+waveform.status());
        Audio audio;audio.open(true,{},false);const auto load=preparedPlaybackLoader(audio,library,[]{});
        if(!load(track->path,true,0))throw std::runtime_error(audio.error);
        const auto loadDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!audio.loaded&&audio.error.empty()&&std::chrono::steady_clock::now()<loadDeadline){audio.poll();Sleep(5);}
        if(!audio.loaded||!audio.paused||std::abs(audio.duration-source.duration)>1)
            throw std::runtime_error("Original playback failed: "+audio.error);
        if(argc==3){
            if(std::string(argv[2])!="--measure")throw std::runtime_error("Unknown fixture operation.");
            const prepared::Frame frames=native.at("total_frames"),rate=native.at("sample_rate");
            const auto count=std::min(frames,rate*3);std::mt19937_64 random(43);
            std::uniform_int_distribution<prepared::Frame> positions(0,frames-count);std::vector<double> durations;
            for(int i=0;i<100;++i){
                const auto start=std::chrono::steady_clock::now();read(positions(random),count);
                durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            }
            std::sort(durations.begin(),durations.end());
            std::cout<<sampling::Json{{"windows_http",true},{"cache","warm filesystem; reused backend process"},{"capture",source.source.capture},
                {"frames_per_read",count},{"sample_count",100},{"source_seconds",source.duration},
                {"ms_p50_p95_p99",{durations[49],durations[94],durations[98]}}}.dump()<<'\n';
        }
        std::cout<<"PASS Windows indexed catalog, exact native PCM, compact peaks and original playback\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

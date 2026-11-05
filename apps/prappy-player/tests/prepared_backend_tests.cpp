#include "remote_catalog.h"
#include "remote_waveform.h"
#include "audio.h"
#include "prepared_requests.h"
#include <iostream>
#include <random>
#include <algorithm>

int main(int argc,char** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("Pass the isolated prepared fixture manifest and optional --measure.");
        const auto fixture=sampling::Json::parse(stateio::read(stateio::pathFromText(argv[1])));
        const auto directory=fs::temp_directory_path()/("prappy-prepared-client-"+sampling::uuid());
        fs::create_directory(directory);
        const auto config=directory/"player.ini";
        stateio::write(config,"directory=C:/intentionally-missing-do-not-scan\nextensions=opus\n");
        stateio::write(directory/"sampling-connection.json",sampling::Json{
            {"url",fixture.at("url")},{"token_file",fixture.at("token_file")}}.dump());
        Library library;ScanProgress progress;
        if(!refreshPreparedCatalog(library,config,{},progress))throw std::runtime_error(library.notice);
        if(library.tracks.size()!=2||progress.hashReads!=0||!library.backend)
            throw std::runtime_error("Catalog did not come from the prepared service.");
        for(const auto& track:library.tracks)
            if(!track.prepared||!track.prepared->ready())
                throw std::runtime_error("Missing prepared native timeline.");
        const auto& source=*library.tracks.front().prepared;
        sampling::Request inspect;inspect.tag="prepared-manifest";inspect.path=source.source.endpoint();
        const auto manifest=sampling::perform(*library.backend,inspect);
        if(!manifest.error.empty())throw std::runtime_error(manifest.error);
        sampling::Request range;range.tag="prepared-pcm";
        range.path=source.source.endpoint()+"/pcm?asset="+source.asset+"&start=100&count=1000";
        range.body=prepared::pcmContext(manifest.body,100,1000);
        const auto reply=sampling::perform(*library.backend,range);
        if(!reply.error.empty()||!reply.preparedPcm||reply.preparedPcm->frames()!=1000)
            throw std::runtime_error("Prepared PCM failed: "+reply.error);
        RemoteWaveform waveform([]{});
        waveform.select(source,*library.backend);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!waveform.value()&&std::chrono::steady_clock::now()<deadline) {
            waveform.collect();Sleep(5);
        }
        if(!waveform.value()||waveform.value()->frames!=static_cast<uint64_t>(source.listening.frames)||waveform.value()->levels[0].empty())
            throw std::runtime_error("Prepared waveform failed: "+waveform.status());
        Audio audio;audio.open(true,{},false);
        const auto load=preparedPlaybackLoader(audio,library,[]{});
        if(!load(library.tracks.front().path,true,0))
            throw std::runtime_error(audio.error);
        const auto loadDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!audio.loaded&&audio.error.empty()&&std::chrono::steady_clock::now()<loadDeadline){audio.poll();Sleep(5);}
        if(!audio.loaded||!audio.paused||audio.duration<14.9)
            throw std::runtime_error("Prepared playback failed: "+audio.error);
        if(argc==3){
            if(std::string(argv[2])!="--measure")throw std::runtime_error("Unknown fixture operation.");
            const auto count=std::min({source.native.frames,source.native.rate*3,
                int64_t{64*1024*1024}/static_cast<int64_t>(source.native.channels*prepared::sampleBytes(source.native.type))});
            std::mt19937_64 random(43);
            std::uniform_int_distribution<int64_t> positions(0,source.native.frames-count);
            sampling::Json measured={{"frames_per_read",count},{"sample_count",100},{"source_seconds",source.duration}};
            for(const bool cold:{false,true}){
                std::vector<double> durations;
                for(int i=0;i<100;++i){
                    if(cold){
                        sampling::Request evict;evict.tag="fixture-evict";evict.method="POST";
                        evict.path="/_fixture/evict?asset="+source.asset;
                        const auto result=sampling::perform(*library.backend,evict);
                        if(!result.error.empty())throw std::runtime_error(result.error);
                    }
                    const auto origin=positions(random);
                    range.path=source.source.endpoint()+"/pcm?asset="+source.asset+"&start="+std::to_string(origin)+"&count="+std::to_string(count);
                    range.body=prepared::pcmContext(manifest.body,origin,count);
                    const auto began=std::chrono::steady_clock::now();
                    const auto result=sampling::perform(*library.backend,range);
                    if(!result.error.empty()||!result.preparedPcm)throw std::runtime_error(result.error);
                    durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count());
                }
                std::sort(durations.begin(),durations.end());
                measured[cold?"advisory_evicted_ms_p50_p95_p99":"warm_ms_p50_p95_p99"]={durations[49],durations[94],durations[98]};
            }
            std::cout<<measured.dump()<<'\n';
        }
        std::cout<<"PASS Windows prepared catalog: two Ready sources, no source scan or hashes\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

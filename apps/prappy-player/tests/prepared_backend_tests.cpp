#include "remote_catalog.h"
#include "remote_waveform.h"
#include "audio.h"
#include "prepared_requests.h"
#include <iostream>

int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Pass the isolated prepared fixture manifest.");
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
            if(!track.prepared||!track.prepared->ready()||track.prepared->native.frames!=fixture.at("previous_capture").at("timeline").at("frame_count"))
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
        waveform.select(library.tracks.front(),*library.backend);
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
        std::cout<<"PASS Windows prepared catalog: two Ready sources, no source scan or hashes\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

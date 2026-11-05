#include "remote_catalog.h"
#include "audio.h"
#include "debug_log.h"

bool refreshPreparedCatalog(Library& library,const fs::path& config,std::stop_token stop,ScanProgress& progress) {
    const auto began=debuglog::Clock::now();
    try {
        const auto settings=readConfig(config);
        sampling::Connection connection;
        connection.credentialFile=L"\\\\wsl.localhost\\ubuntu26-dev\\home\\home\\.config\\audioharvest\\server.token";
        const auto connectionFile=config.parent_path()/"sampling-connection.json";
        if(fs::exists(connectionFile)) {
            const auto saved=sampling::Json::parse(stateio::read(connectionFile));
            connection.url=saved.at("url");
            connection.credentialFile=stateio::pathFromText(saved.at("token_file"));
        }
        connection.startWorkstation=connection.url=="http://127.0.0.1:8765";
        sampling::Request request;request.tag="catalog";request.path="/v1/catalog";
        struct Signal {std::mutex mutex;std::condition_variable_any wake;std::atomic<uint64_t> epoch{0};};
        const auto signal=std::make_shared<Signal>();
        sampling::Client client(connection,[signal]{++signal->epoch;signal->wake.notify_all();});
        client.submit(request);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
        sampling::Reply reply;
        while(true) {
            const auto epoch=signal->epoch.load();
            auto replies=client.collect();
            if(!replies.empty()){reply=std::move(replies.front());break;}
            std::unique_lock lock(signal->mutex);
            if(!signal->wake.wait_until(lock,stop,deadline,[&]{return signal->epoch.load()!=epoch;}))
                throw std::runtime_error(stop.stop_requested()?"Catalog request cancelled.":"Prepared catalog request timed out.");
        }
        if(!reply.error.empty())throw std::runtime_error(reply.error);
        if(reply.status!=200)throw std::runtime_error("Prepared catalog is unavailable.");
        connection=sampling::authorize(std::move(connection),stop);
        auto sources=prepared::parseCatalog(reply.body);
        std::vector<Track> tracks;
        std::set<fs::path> roots;
        std::set<std::string> extensions;
        std::map<std::wstring,std::string> labels;
        tracks.reserve(sources.size());
        for(auto& source:sources) {
            if(stop.stop_requested())throw std::runtime_error("Catalog request cancelled.");
            Track track;
            track.sourceRoot=fs::absolute(config.parent_path())/"sources"/source.source.library;
            track.path=track.sourceRoot/(source.source.capture+".source");
            track.id=key(track.path);track.name=source.title;track.directory=source.libraryName;
            track.type=source.codec;track.bytes=source.originalBytes;
            track.label=source.libraryName+" / "+(source.artist.empty()?std::string{}:source.artist+" - ")+source.title;
            labels.insert_or_assign(key(track.sourceRoot),source.libraryName);
            labels.insert_or_assign(track.id,track.label);
            track.duration=source.duration;track.offline=source.state=="Offline";
            roots.insert(track.sourceRoot);extensions.insert("."+source.codec);
            track.prepared=std::move(source);tracks.push_back(std::move(track));
            ++progress.found;
        }
        library.tracks=std::move(tracks);
        library.roots.assign(roots.begin(),roots.end());library.extensions=std::move(extensions);
        library.labels=std::move(labels);
        library.backendDistribution=reply.body.value("wsl_distribution",sampling::Json()).is_string()?
            reply.body.at("wsl_distribution").get<std::string>():std::string{};
        library.backend=std::make_shared<sampling::Connection>(std::move(connection));
        library.historyLimit=settings.historyLimit;
        library.addNewListeningHistoryEntryWhenReplayingHistoryItem=settings.addNewListeningHistoryEntryWhenReplayingHistoryItem;
        library.rebuildSearch();library.sort();library.notice.clear();
        debuglog::write("catalog.ready","ms="+std::to_string(debuglog::elapsed(began))+" sources="+std::to_string(library.tracks.size()));
        return true;
    } catch(const std::exception& error) {
        library.notice=error.what();debuglog::write("catalog.error","ms="+std::to_string(debuglog::elapsed(began))+" error="+library.notice);return false;
    }
}

std::optional<fs::path> preparedOriginalLocation(const Track& track,const Library& library) {
    if(!track.prepared||!library.backend||library.backendDistribution.empty()||
       (library.backend->url!="http://127.0.0.1:8765"&&!library.backend->url.starts_with("http://127.0.0.1:")))return {};
    auto path=track.prepared->originalPath;
    std::replace(path.begin(),path.end(),'/','\\');
    return stateio::pathFromText("\\\\wsl.localhost\\"+library.backendDistribution+path);
}

std::function<bool(const fs::path&,bool,double)> preparedPlaybackLoader(Audio& audio,Library& library,std::function<void()> notify) {
    struct Promotions {sampling::Connection connection;std::unique_ptr<sampling::Client> client;};
    const auto promotions=std::make_shared<Promotions>();
    return [&audio,&library,notify=std::move(notify),promotions](const fs::path& path,bool paused,double position) {
        const auto id=key(path);
        const auto found=std::find_if(library.tracks.begin(),library.tracks.end(),
            [&](const auto& track){return Library::matches(track,id);});
        if(!library.backend||found==library.tracks.end()||!found->prepared)
            throw std::runtime_error("Recording is absent from the prepared library.");
        const auto& source=*found->prepared;
        if(!source.ready()) {
            const auto& connection=*library.backend;
            if(!promotions->client||promotions->connection.url!=connection.url||promotions->connection.token!=connection.token) {
                promotions->connection=connection;
                promotions->client=std::make_unique<sampling::Client>(connection,notify);
            }
            promotions->client->collect();
            sampling::Request request;request.tag="prepared-promote";request.method="POST";
            request.path=source.source.endpoint()+"/promote";promotions->client->submit(std::move(request));
            throw prepared::NotReady("Recording preparation: "+source.state);
        }
        const auto loaded=audio.play(path,paused,position,library.backend->url+source.source.endpoint()+
                                    "/playback?asset="+source.asset+"&timeline="+source.timeline,library.backend->token);
        if(loaded)audio.preparedSource=std::make_shared<prepared::CatalogSource>(source);
        return loaded;
    };
}

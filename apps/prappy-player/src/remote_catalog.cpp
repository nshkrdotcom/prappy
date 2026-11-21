#include "remote_catalog.h"
#include "audio.h"
#include "debug_log.h"
#include "evaluation_state.h"

namespace {
std::string scopeOf(const sampling::Connection& connection){
    return connection.url+"\n"+utf8(connection.credentialFile);
}
bool validValidator(const std::string& tag){
    return tag.size()==66&&tag.front()=='"'&&tag.back()=='"'&&
        tag.substr(1,64).find_first_not_of("0123456789abcdef")==std::string::npos;
}
void populate(Library& library,const fs::path& config,const sampling::Json& body,
              sampling::Connection connection,std::stop_token stop,ScanProgress& progress){
    const auto settings=readConfig(config);
        auto sources=prepared::parseCatalog(body);
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
            track.sourceId=L"\\"+key(fs::path("sources")/source.source.library/(source.source.capture+".source"));
            track.id=key(track.path);track.name=source.title;track.directory=source.libraryName;
            track.type=source.codec;track.bytes=source.originalBytes;
            track.label=source.libraryName+" / "+(source.artist.empty()?std::string{}:source.artist+" - ")+source.title;
            labels.insert_or_assign(key(track.sourceRoot),source.libraryName);
            labels.insert_or_assign(track.id,track.label);labels.insert_or_assign(track.sourceId,track.label);
            track.duration=source.duration;track.offline=source.state=="Offline";
            roots.insert(track.sourceRoot);extensions.insert("."+source.codec);
            track.prepared=std::move(source);tracks.push_back(std::move(track));
            ++progress.found;
        }
        library.tracks=std::move(tracks);
        library.roots.assign(roots.begin(),roots.end());library.extensions=std::move(extensions);
        library.labels=std::move(labels);
        library.backendDistribution=body.value("wsl_distribution",sampling::Json()).is_string()?
            body.at("wsl_distribution").get<std::string>():std::string{};
        library.backend=std::make_shared<sampling::Connection>(std::move(connection));
        library.historyLimit=settings.historyLimit;
        library.addNewListeningHistoryEntryWhenReplayingHistoryItem=settings.addNewListeningHistoryEntryWhenReplayingHistoryItem;
        library.rebuildSearch();library.sort();library.notice.clear();
}
}

std::string serializeResumeSource(const prepared::CatalogSource& source){
    sampling::Json row={{"library_id",source.source.library},{"capture_id",source.source.capture},
        {"title",source.title},{"artist",source.artist},{"library_name",source.libraryName},
        {"original_sha256",source.asset},{"original_path",source.originalPath},
        {"original_bytes",source.originalBytes},{"state",source.state},
        {"media",{{"codec",source.codec},{"duration_seconds",source.duration},
                  {"sample_rate",source.native.rate},{"channels",source.native.channels}}}};
    return sampling::Json{{"protocol",3},{"timeline",source.timeline},{"items",sampling::Json::array({std::move(row)})}}.dump();
}

std::optional<prepared::CatalogSource> readResumeSource(const fs::path& path){
    try{
        std::error_code error;
        if(fs::file_size(path,error)>64*1024||error)return {};
        const auto record=sampling::Json::parse(stateio::read(path));
        auto sources=prepared::parseCatalog(record);
        const auto timeline=record.at("timeline").get<std::string>();
        if(sources.size()==1&&prepared::SourceRef::valid(timeline,"tl_")){
            sources.front().timeline=timeline;return std::move(sources.front());
        }
    }catch(const std::exception& error){debuglog::write("startup.resume.error",error.what());}
    return {};
}

sampling::Connection preparedConnection(const fs::path& directory){
        sampling::Connection connection;
        connection.credentialFile=L"\\\\wsl.localhost\\ubuntu26-dev\\home\\home\\.config\\audioharvest\\server.token";
        const auto connectionFile=directory/"sampling-connection.json";
        if(fs::exists(connectionFile)) {
            const auto saved=sampling::Json::parse(stateio::read(connectionFile));
            connection.url=saved.at("url");
            connection.credentialFile=stateio::pathFromText(saved.at("token_file"));
        }
        stateio::evaluationConnection(directory,connection.url,connection.credentialFile);
        connection.startWorkstation=connection.url=="http://127.0.0.1:8765";
        connection.resumeDirectory=directory/"song-pcm-v1";
    return connection;
}

bool refreshPreparedCatalog(Library& library,const fs::path& config,std::stop_token stop,ScanProgress& progress) {
    const auto began=debuglog::Clock::now();
    try {
        auto connection=preparedConnection(config.parent_path());
        sampling::Request request;request.tag="catalog";request.path="/v1/browse-catalog";
        const auto scope=scopeOf(connection);
        if(library.catalogScope==scope)request.catalogValidator=library.catalogValidator;
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
        if(reply.status==304&&validValidator(request.catalogValidator)&&reply.catalogValidator==request.catalogValidator){
            debuglog::write("catalog.unchanged","ms="+std::to_string(debuglog::elapsed(began)));
            library.notice.clear();library.catalogCurrent=true;return false;
        }
        if(reply.status!=200||!validValidator(reply.catalogValidator))
            throw std::runtime_error("Prepared catalog reply is unavailable or invalid.");
        populate(library,config,reply.body,connection,stop,progress);
        library.catalogScope=scope;library.catalogValidator=reply.catalogValidator;library.catalogCurrent=true;
        try{
            stateio::write(config.parent_path()/"browse-snapshot-v1.json",
                sampling::Json{{"scope",scope},{"validator",reply.catalogValidator},{"catalog",std::move(reply.body)}}.dump(),false);
        }catch(const std::exception& error){debuglog::write("catalog.cache.error",error.what());}
        debuglog::write("catalog.ready","ms="+std::to_string(debuglog::elapsed(began))+" sources="+std::to_string(library.tracks.size()));
        return true;
    } catch(const std::exception& error) {
        library.notice=error.what();debuglog::write("catalog.error","ms="+std::to_string(debuglog::elapsed(began))+" error="+library.notice);return false;
    }
}

bool restorePreparedCatalog(Library& library,const fs::path& config,std::stop_token stop,ScanProgress& progress){
    try{
        auto connection=preparedConnection(config.parent_path());
        const auto scope=scopeOf(connection);
        const auto path=config.parent_path()/"browse-snapshot-v1.json";
        if(!fs::exists(path))return false;
        auto saved=sampling::Json::parse(stateio::read(path));
        const auto validator=saved.at("validator").get<std::string>();
        if(saved.at("scope")!=scope||!validValidator(validator))return false;
        populate(library,config,saved.at("catalog"),std::move(connection),stop,progress);
        library.catalogScope=scope;library.catalogValidator=validator;
        debuglog::write("catalog.local","sources="+std::to_string(library.tracks.size()));
        return true;
    }catch(const std::exception& error){
        debuglog::write("catalog.cache.error",error.what());return false;
    }
}
std::optional<fs::path> preparedOriginalLocation(const Track& track,const Library& library) {
    if(!track.prepared||!library.backend||library.backendDistribution.empty()||
       (library.backend->url!="http://127.0.0.1:8765"&&!library.backend->url.starts_with("http://127.0.0.1:")))return {};
    auto path=track.prepared->originalPath;
    std::replace(path.begin(),path.end(),'/','\\');
    return stateio::pathFromText("\\\\wsl.localhost\\"+library.backendDistribution+path);
}

std::function<bool(const fs::path&,bool,double)> preparedPlaybackLoader(Audio& audio,Library& library,std::function<void()>) {
    return [&audio,&library](const fs::path& path,bool paused,double position) {
        const auto id=key(path);
        const auto found=std::find_if(library.tracks.begin(),library.tracks.end(),
            [&](const auto& track){return Library::matches(track,id);});
        const auto* source=found!=library.tracks.end()&&found->prepared?&*found->prepared:nullptr;
        if(!source&&library.resumeSource){
            const auto& resume=*library.resumeSource;
            const auto suffix=L"\\"+key(fs::path("sources")/resume.source.library/(resume.source.capture+".source"));
            if(id.ends_with(suffix))source=&resume;
        }
        if(!library.backend||!source)
            throw std::runtime_error("Recording is absent from the indexed library.");
        if(!source->ready())throw prepared::NotReady("Recording is unavailable: "+source->state);
        return audio.playNative(path,*source,*library.backend,paused,position);
    };
}

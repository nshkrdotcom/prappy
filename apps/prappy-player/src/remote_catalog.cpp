#include "remote_catalog.h"

bool refreshPreparedCatalog(Library& library,const fs::path& config,std::stop_token stop,ScanProgress& progress) {
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
        connection=sampling::authorize(std::move(connection),stop);
        sampling::Request request;request.tag="catalog";request.path="/v1/catalog";
        auto reply=sampling::perform(connection,request,stop);
        if(!reply.error.empty())throw std::runtime_error(reply.error);
        if(reply.status!=200)throw std::runtime_error("Prepared catalog is unavailable.");
        auto sources=prepared::parseCatalog(reply.body);
        std::vector<Track> tracks;
        std::set<fs::path> roots;
        std::set<std::string> extensions;
        tracks.reserve(sources.size());
        for(auto& source:sources) {
            if(stop.stop_requested())throw std::runtime_error("Catalog request cancelled.");
            Track track;
            track.sourceRoot=fs::absolute(config.parent_path())/"sources"/source.source.library;
            track.path=track.sourceRoot/(source.source.capture+".source");
            track.id=key(track.path);track.name=source.title;track.directory=source.libraryName;
            track.type=source.codec;track.bytes=source.originalBytes;
            track.label=source.libraryName+" / "+(source.artist.empty()?std::string{}:source.artist+" - ")+source.title;
            track.duration=source.duration;track.offline=source.state=="Offline";
            roots.insert(track.sourceRoot);extensions.insert("."+source.codec);
            track.prepared=std::move(source);tracks.push_back(std::move(track));
            ++progress.found;
        }
        library.tracks=std::move(tracks);library.files=library.tracks;
        library.roots.assign(roots.begin(),roots.end());library.extensions=std::move(extensions);
        library.verified.clear();library.backend=std::make_shared<sampling::Connection>(std::move(connection));
        library.historyLimit=settings.historyLimit;
        library.addNewListeningHistoryEntryWhenReplayingHistoryItem=settings.addNewListeningHistoryEntryWhenReplayingHistoryItem;
        library.rebuildSearch();library.sort();library.notice.clear();
        return true;
    } catch(const std::exception& error) {
        library.notice=error.what();return false;
    }
}

#include "sample_workspace.h"
#include "state_io.h"
#include "sample_source_registration.h"
#include "imgui.h"
#include "implot.h"
#include "waveform_view.h"
#include <fstream>
#include <sstream>
#include <iomanip>
namespace sampling {
namespace {
bool input(const char* label,std::string& value,size_t limit=4096) {
    std::array<char,4096> buffer{};std::copy_n(value.data(),std::min(value.size(),buffer.size()-1),buffer.data());
    if(ImGui::InputText(label,buffer.data(),std::min(limit,buffer.size()))){value=buffer.data();return true;}return false;
}
std::string encode(const std::string& s) {
    std::string out;const char* hex="0123456789ABCDEF";
    for(unsigned char c:s) {if(std::isalnum(c)||c=='-'||c=='_'||c=='.')out+=static_cast<char>(c);else{out+='%';out+=hex[c>>4];out+=hex[c&15];}}return out;
}
std::string endpoint(const std::string& library){return "/v1/libraries/"+library;}
Json strings(const std::string& text){Json out=Json::array();std::istringstream in(text);for(std::string s;std::getline(in,s,',');){s=trim(s);if(!s.empty())out.push_back(s);}return out;}
std::string show(const Json& j){if(j.is_string())return j.get<std::string>();if(j.is_array()){std::string out;for(const auto& v:j){if(!out.empty())out+=", ";out+=v.get<std::string>();}return out;}return j.is_null()?"":j.dump();}
int timeLabel(double x,char* buffer,int size,void*){return std::snprintf(buffer,static_cast<size_t>(size),"%.4fs",x/48000.);}
}
Workspace::Workspace(fs::path d,std::function<void()> n):directory(std::move(d)),notify(std::move(n)){
    tokenFile=utf8(fs::path(L"\\\\wsl.localhost\\ubuntu26-dev\\home\\home\\.config\\audioharvest\\server.token"));try{restore();}catch(const std::exception& e){storageBlocked=true;storageError=e.what();}
}
Workspace::~Workspace(){try{persist();}catch(const std::exception& e){OutputDebugStringA(e.what());}}
void Workspace::get(std::string tag,std::string path){if(client){Json context=tag=="sampleMetadata"?draftSnapshot():Json();client->submit({std::move(tag),"GET",std::move(path),context,generation,{}});}}
void Workspace::connect(){
    connectionAttempted=true;stopTransport();
    try{
        connection.url=serverUrl;
        connection.token.clear();connection.credentialFile=stateio::pathFromText(tokenFile);
        connection.startWorkstation=serverUrl=="http://127.0.0.1:8765";
        client=std::make_unique<Client>(connection,notify);get("libraries","/v1/libraries");notice="Connecting...";
        stateio::write(directory/"sampling-connection.json",Json{{"url",serverUrl},{"token_file",tokenFile},{"library",library}}.dump(2));
    }catch(const std::exception& e){notice=e.what();}
}
void Workspace::browse(){
    if(library.empty())return;
    const std::string route=tab==0?"/recordings":tab==1?"/regions":"/collections";
    browsePath=endpoint(library)+route+"?q="+encode(libraryQuery)+"&facet="+encode(facet)+"&value="+encode(facetValue)+"&limit=100&offset="+std::to_string(browseOffset);
    get("browse",browsePath);
    get("collections",endpoint(library)+"/collections");
}
void Workspace::openCapture(Json item,std::optional<Span> selected){
    stash();stopTransport();++generation;++waveformRevision;overviewBins.clear();envelopePcm=false;envelope.clear();displayEnvelope.clear();projectedGain=-1;analysisJob.clear();
    capture=std::move(item);sourceLibrary=library;activeDraft.clear();region=nullptr;selectionDefined=false;sourceRegions=Json::array();
    pcm.clear();pcmStart=pcmEnd=0;beats.clear();onsets.clear();anchors.clear();overview=nullptr;timing=nullptr;gridId.clear();gridRevision=0;
    const Frame total=capture.at("timeline").at("frame_count").get<Frame>();
    auto& ws=sampleLibrary.open(sourceRef(),total);const auto active=ws.active;const auto view=ws.view;
    editor.reset(total,{0,std::min<Frame>(total,Rate*4)});metadata=capture.value("metadata",Json::object());metadataTarget.clear();metadataRevision=0;metadataTargetRevision=1;
    metadataFields(metadata.value("effective",Json::object()));regionName=capture.at("recording").value("title","Sample")+" sample";kind="unknown";
    if(!active.empty())activateDraft(active);
    transport.scope=static_cast<PlaybackScope>(view.value("scope",0));transport.loop=view.value("loop",true);transport.follow=view.value("follow",false);
    snapMode=view.value("snap",0);amplitude.mode=static_cast<AmplitudeMode>(view.value("amplitude_mode",0));amplitude.gain=AmplitudeScale::bounded(view.value("amplitude_gain",1.));
    viewportStart=view.value("viewport_start",0.);viewportEnd=view.value("viewport_end",static_cast<double>(std::min<Frame>(total,Rate*6)));
    if(selected)newDraft(*selected);
    transport.range=transport.bounds(editor.span,total);transport.cursor=transport.range.start;fit=true;
    if(!preparation.match.is_null()&&preparation.match.at("library_id")==sourceLibrary&&preparation.match.at("capture").at("id")==capture.at("id")){
        if(!preparation.pcm.empty()){pcm=preparation.pcm;pcmStart=preparation.pcmStart;pcmEnd=pcmStart+static_cast<Frame>(pcm.size()/2);++waveformRevision;}
        if(!preparation.regions.is_null()){sourceRegions=preparation.regions.at("items");sourceRegionTotal=preparation.regions.at("total");}
        // Overview processing goes through the same reply handler when fetched; PCM is the latency-critical cache.
    }
    loadDetails();fetchSourceRegions();draftDirty=true;persistDue=std::chrono::steady_clock::now();
}
void Workspace::loadDetails(bool refresh){
    if(capture.is_null())return;
    const auto base=endpoint(library)+"/captures/"+capture.at("id").get<std::string>();
    if(overview.is_null()||refresh)get("overview",base+"/overview");
    if(timing.is_null()||refresh)get("timing",base+"/timing");
    const auto range=waveformRange(viewportStart,viewportEnd,editor.total);
    const Frame start=range.start,end=range.end;
    if(end-start<=Rate*60)get("pcm",base+"/pcm?start="+std::to_string(start)+"&end="+std::to_string(end)+"&timeline="+capture.at("timeline_id").get<std::string>());
}
void Workspace::metadataFields(const Json& values){editedFacets.clear();fields=Json::object();for(const char* key:{"title","artist","album","date","genres","moods","instruments","roles","tags","uses","key","tempo_bpm","energy","notes","character"})fields[key]=values.contains(key)?show(values[key]):"";}
Json Workspace::humanFacts(bool forSample)const{
    if(forSample && !metadataTarget.empty() && (region.is_null() || metadataTarget!=region.at("id").get<std::string>()))return Json::array();
    Json chosen=Json::object();
    for(const auto& fact:(metadataTarget.empty()?Json::array():metadata.value("facts",Json::array())))if(fact.value("origin","")=="human")chosen[fact.at("facet").get<std::string>()]=fact;
    for(auto it=fields.begin();it!=fields.end();++it){
        if(!editedFacets.contains(it.key()))continue;
        const auto text=trim(it.value().get<std::string>());
        if(text.empty()){chosen[it.key()]={{"facet",it.key()},{"value",nullptr},{"origin","human"},{"state","rejected"}};continue;}
        Json value=text;
        if(it.key()=="tempo_bpm"||it.key()=="energy")value=std::stod(text);
        else if(it.key()=="genres"||it.key()=="moods"||it.key()=="instruments"||it.key()=="roles"||it.key()=="tags"||it.key()=="uses")value=strings(text);
        chosen[it.key()]={{"facet",it.key()},{"value",value},{"origin","human"},{"state","accepted"}};
    }
    Json facts=Json::array();for(const auto& fact:chosen)facts.push_back(fact);return facts;
}
void Workspace::command(std::string operation,Json payload){
    if(!client||library.empty()){notice="Connect and select a library first.";return;}
    Json body={{"request_id",uuid()},{"operation",operation},{"payload",std::move(payload)}};
    Json entry={{"library",library},{"command",body},{"draft",draftSnapshot()}};
    if(operation=="region.save"){
        if(activeDraft.empty())newDraft(editor.span);
        stash();body=sampleLibrary.beginSave(sourceRef(),activeDraft,body);
        entry={{"library",sourceLibrary},{"command",body},{"draft",draftSnapshot()},{"source",sourceRef().json()},{"draft_id",activeDraft}};
    }
    const auto duplicate=std::find_if(pending.begin(),pending.end(),[&](const auto& row){return row.at("command").at("request_id")==body.at("request_id");});
    if(duplicate==pending.end())pending.push_back(entry);
    persist();
    client->submit({"command","POST",endpoint(entry.at("library"))+"/commands",body,generation,{}});
    notice="Saving command...";
}
Json Workspace::draftSnapshot()const{
    return {{"span",{editor.span.start,editor.span.end}},{"name",regionName},{"kind",kind},{"fields",fields},{"edited_facets",editedFacets},{"metadata_target",metadataTarget}};
}
void Workspace::discardConflicts(){
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& row){return row.at("library")==library&&row.value("conflict",false)&&(!row.contains("draft_id")||row.at("draft_id")==activeDraft);}),pending.end());
}
SourceRef Workspace::sourceRef()const{return {sourceLibrary,capture.at("id"),capture.at("timeline_id")};}
DraftValue Workspace::draftValue()const{
    return {editor.span,{{"name",regionName},{"kind",kind},{"fields",fields},{"edited_facets",editedFacets},
        {"metadata",metadata},{"metadata_target",metadataTarget},{"metadata_target_revision",metadataTargetRevision},{"metadata_revision",metadataRevision}}};
}
void Workspace::stash(){
    if(capture.is_null()||sourceLibrary.empty())return;
    auto& ws=sampleLibrary.open(sourceRef(),editor.total);
    if(!activeDraft.empty())sampleLibrary.edit(sourceRef(),activeDraft,draftValue());
    ws.active=activeDraft;
    ws.view={{"capture",capture},{"viewport_start",viewportStart},{"viewport_end",viewportEnd},{"snap",snapMode},
        {"loop",transport.loop},{"scope",static_cast<int>(transport.scope)},{"follow",transport.follow},
        {"amplitude_mode",static_cast<int>(amplitude.mode)},{"amplitude_gain",amplitude.gain}};
}
void Workspace::activateDraft(const std::string& id){
    stopTransport();activeDraft=id;auto& ws=sampleLibrary.at(sourceRef());ws.active=id;
    const auto& d=ws.drafts.at(id);const auto& a=d.value.attributes;
    editor.reset(ws.total,d.value.span);editor.dirty=d.dirty();selectionDefined=true;
    regionName=a.value("name",std::string("Sample"));kind=a.value("kind",std::string("unknown"));
    fields=a.value("fields",Json::object());editedFacets=a.value("edited_facets",Json::array()).get<std::set<std::string>>();
    metadata=a.value("metadata",Json::object());metadataTarget=a.value("metadata_target",std::string{});
    metadataTargetRevision=a.value("metadata_target_revision",1);metadataRevision=a.value("metadata_revision",0);
    region=d.region?Json{{"id",d.region->id},{"revision",d.region->revision},{"capture_id",sourceRef().capture},{"timeline_id",sourceRef().timeline},
        {"name",regionName},{"kind",kind},{"span",{{"start_frame",d.value.span.start},{"end_frame",d.value.span.end}}}}:Json();
    viewportStart=static_cast<double>(std::max<Frame>(0,editor.span.start-Rate));viewportEnd=static_cast<double>(std::min(editor.total,editor.span.end+Rate));
    fit=true;transport.range=transport.bounds(editor.span,editor.total);transport.cursor=transport.range.start;loadDetails();draftDirty=true;
    if(d.region&&!d.dirty()&&!d.saving)get("sampleMetadata",endpoint(sourceLibrary)+"/metadata/"+d.region->id+"?revision="+std::to_string(d.region->revision));
}
void Workspace::newDraft(Span span,bool variant){
    stash();auto value=draftValue();value.span=span;
    value.attributes["metadata_target"]="";value.attributes["metadata_revision"]=0;
    const auto id=sampleLibrary.select(sourceRef(),value,uuid(),variant);activateDraft(id);
    transport.scope=PlaybackScope::Selection;transport.loop=true;transport.range=span;transport.cursor=span.start;
}
void Workspace::fetchSourceRegions(size_t offset){
    if(capture.is_null()||!client)return;sourceRegionOffset=offset;
    get("sourceRegions",endpoint(sourceLibrary)+"/captures/"+capture.at("id").get<std::string>()+"/regions?timeline="+capture.at("timeline_id").get<std::string>()+"&limit=100&offset="+std::to_string(offset));
}
void Workspace::chooseRegion(const Json& item){
    stash();DraftValue value{{item.at("span").at("start_frame"),item.at("span").at("end_frame")},
        {{"name",item.at("name")},{"kind",item.at("kind")},{"fields",Json::object()},{"edited_facets",Json::array()},
         {"metadata",item.value("metadata",Json::object())},{"metadata_target",item.at("id")},
         {"metadata_target_revision",item.at("revision")},{"metadata_revision",item.value("metadata",Json::object()).value("revision",0)}}};
    const auto id=sampleLibrary.selectSaved(sourceRef(),item,value,uuid());transport.scope=PlaybackScope::Selection;activateDraft(id);
    if(fields.empty())metadataFields(metadata.value("effective",Json::object()));
}
void Workspace::persist(){
    if(storageBlocked)return;
    stash();
    Json data={{"workspaces",sampleLibrary.json()},{"active_source",capture.is_null()?Json():sourceRef().json()},
        {"open",open},{"commands",pending},{"browser_library",library},{"search",libraryQuery},{"facet",facet},{"facet_value",facetValue},
        {"tab",tab},{"offset",browseOffset},{"gain",previewGain},{"source_handoff",handoff.snapshot()},
        {"source_handoff_digest",pendingMark?pendingMark->digest:""},
        {"source_handoff_request",pendingMark?pendingMark->importRequest:""},
        {"source_handoff_library",pendingMark?pendingMark->importLibrary:""}};
    stateio::write(directory/"sample-workspaces.json",data.dump(2));draftDirty=false;persistedSpan=editor.span;
}
void Workspace::restore(){
    try{auto config=Json::parse(stateio::read(directory/"sampling-connection.json"));serverUrl=config.at("url");tokenFile=config.at("token_file");library=config.value("library","");}catch(const std::exception&){}
    const auto path=directory/"sample-workspaces.json";if(!fs::exists(path))return;
    // Invalid state must be visible, never silently replaced by an empty library.
    auto data=Json::parse(stateio::read(path));sampleLibrary=SampleLibrary::parse(data.at("workspaces"));pending=data.at("commands");open=data.at("open");
    library=data.at("browser_library");libraryQuery=data.at("search");facet=data.at("facet");facetValue=data.at("facet_value");tab=data.at("tab");browseOffset=data.at("offset");previewGain=data.at("gain");
    handoff.restore(data.value("source_handoff",Json()));
    if(const auto& intent=handoff.current()){
        pendingMark=PlayingSongMark{stateio::pathFromText(intent->path),static_cast<Frame>(std::llround(intent->cue*Rate)),
            intent->ticket,false,false,intent->selection,intent->duration,intent->action};
        pendingMark->digest=data.value("source_handoff_digest","");
        pendingMark->importRequest=data.value("source_handoff_request","");
        pendingMark->importLibrary=data.value("source_handoff_library","");
    }
    if(data.at("active_source").is_null())return;
    const auto source=SourceRef::parse(data.at("active_source"));auto& ws=sampleLibrary.at(source);sourceLibrary=source.library;
    capture=ws.view.at("capture");
    if(capture.at("id")!=source.capture||capture.at("timeline_id")!=source.timeline||capture.at("timeline").at("frame_count")!=ws.total)throw std::runtime_error("Workspace capture does not match its canonical source.");
    editor.reset(ws.total,{0,std::min<Frame>(Rate*4,ws.total)});
    transport.scope=static_cast<PlaybackScope>(ws.view.value("scope",0));transport.loop=ws.view.value("loop",true);transport.follow=ws.view.value("follow",false);
    snapMode=ws.view.value("snap",0);amplitude.mode=static_cast<AmplitudeMode>(ws.view.value("amplitude_mode",0));amplitude.gain=AmplitudeScale::bounded(ws.view.value("amplitude_gain",1.));
    if(!ws.active.empty())activateDraft(ws.active);
    viewportStart=ws.view.value("viewport_start",0.);viewportEnd=ws.view.value("viewport_end",static_cast<double>(Rate*4));fit=true;
}
void Workspace::mark(Audio& audio,std::optional<SecondsSpan> selection,OpenSource action){
    if(storageBlocked){open=true;return;}
    if(audio.current.empty()||audio.duration<=0){notice="No song is loaded in the player.";return;}
    const auto intent=handoff.begin(utf8(audio.current),selection?OpenSource::Selection:action,audio.displayPosition(),audio.duration,selection);
    open=true;audio.setPaused(true);stopTransport();++generation;
    pendingMark=PlayingSongMark{audio.current,static_cast<Frame>(std::llround(intent.cue*Rate)),intent.ticket,false,false,selection,intent.duration,intent.action};markMatches=Json::array();
    pendingMark->digest=preparation.knownDigest(intent.path,audio.loadedSerial);
    notice="Opening the playing song...";
    persist();
    if(!client||libraries.empty())connect();else submitMark();
}
void Workspace::submitMark(){
    if(!pendingMark||pendingMark->submitted||pendingMark->failed||!client||libraries.empty())return;
    pendingMark->submitted=true;notice="Finding the playing song in your libraries...";
    client->submit({"hash","GET",utf8(pendingMark->path),{{"mark",pendingMark->ticket}},generation,{}});
}
void Workspace::importMarkedSource(){
    if(!pendingMark||!client)return;
    const auto target=pendingMark->importLibrary.empty()?library:pendingMark->importLibrary;
    const auto enabled=std::any_of(libraries.begin(),libraries.end(),[&](const auto& item){
        return item.at("id")==target&&item.value("source_import_enabled",false);
    });
    const auto path=sourceImportPath(pendingMark->path,serverUrl);
    if(!enabled||!path){
        pendingMark->failed=true;
        notice=!enabled?"This server has source registration disabled. Your selected range is retained.":
            "The connected server cannot access this source path. Your selected range is retained.";
        persist();return;
    }
    if(pendingMark->importRequest.empty())pendingMark->importRequest=uuid();
    pendingMark->importLibrary=target;
    const Json body={{"request_id",pendingMark->importRequest},{"operation","source.import"},
        {"payload",{{"path",*path},{"sha256",pendingMark->digest}}}};
    persist();notice="Registering this recording for editing; the original and selected range are preserved...";
    client->submit({"source-import","POST",endpoint(target)+"/commands",body,generation,{}});
}
void Workspace::finishMark(const Json& match){
    if(!pendingMark)return;
    const SourceRef source{match.at("library_id"),match.at("capture").at("id"),match.at("capture").at("timeline_id")};
    std::optional<ResolvedIntent> resolved;
    try{resolved=handoff.resolve(pendingMark->ticket,source,match.at("capture").at("timeline").at("frame_count"));}
    catch(const std::exception& e){pendingMark->failed=true;notice=e.what();return;}
    if(!resolved)return;
    pendingMark.reset();markMatches=Json::array();library=source.library;
    openCapture(match.at("capture"),resolved->selection);
    if(!resolved->selection&&resolved->intent.action==OpenSource::Cue){transport.cursor=resolved->cue;const auto cue=static_cast<double>(resolved->cue);viewportStart=std::max(0.,cue-Rate*3);viewportEnd=std::min(static_cast<double>(editor.total),cue+Rate*3);fit=true;loadDetails();}
    notice=resolved->selection?"Selection opened in sample editor.":"Song opened in sample editor.";
}
void Workspace::stopTransport(){auditionSubmitted=0;++transportEpoch;auditionPath.clear();audition.stop();transport.stop();}
void Workspace::requestAudition(Frame origin,bool first){
    if(capture.is_null()||!client||origin>=transport.range.end)return;
    const auto end=std::min(transport.range.end,origin+Rate*30);
    auditionPath=endpoint(library)+"/captures/"+capture.at("id").get<std::string>()+"/pcm?start="+std::to_string(origin)+"&end="+std::to_string(end)+"&timeline="+capture.at("timeline_id").get<std::string>();
    if(first&&!preparation.match.is_null()&&preparation.match.at("library_id")==sourceLibrary&&preparation.match.at("capture").at("id")==capture.at("id")&&
       origin>=preparation.pcmStart&&end<=preparation.pcmStart+static_cast<Frame>(preparation.pcm.size()/2)){
        const auto from=static_cast<size_t>(origin-preparation.pcmStart)*2,to=from+static_cast<size_t>(end-origin)*2;
        std::vector<float> samples(preparation.pcm.begin()+static_cast<ptrdiff_t>(from),preparation.pcm.begin()+static_cast<ptrdiff_t>(to));
        audition.start(std::move(samples),origin,transport.range,transport.cursor,transport.effectiveLoop(),transport.state==PlaybackState::Paused);audition.gain(previewGain);
        transport.state=transport.state==PlaybackState::Paused?PlaybackState::Paused:PlaybackState::Playing;auditionPath.clear();++auditionChunks;return;
    }
    client->submit({"audition","GET",auditionPath,{{"epoch",transportEpoch},{"first",first}},generation,{}});
}
void Workspace::beginAudition(Audio& audio,BoundaryCheck check,std::optional<Frame> cue,bool paused){
    if(capture.is_null()||!client)return;
    if(!selectionDefined&&transport.scope!=PlaybackScope::Recording){notice="Choose or create a sample first.";return;}
    audio.setPaused(true);stopTransport();auditionChunks=0;transport.boundary=check;auditionSelection=editor.span;
    transport.range=transport.bounds(editor.span,editor.total);
    transport.cursor=std::clamp(cue.value_or(transport.range.start),transport.range.start,transport.range.end-1);
    transport.state=paused?PlaybackState::Paused:PlaybackState::Loading;
    requestAudition(transport.cursor,true);
}
void Workspace::toggleTransport(Audio& audio){
    if(transport.state==PlaybackState::Playing||transport.state==PlaybackState::Buffering){audition.pause(true);transport.cursor=audition.snapshot().cursor;transport.state=PlaybackState::Paused;}
    else if(transport.state==PlaybackState::Paused){audio.setPaused(true);audition.pause(false);transport.state=auditionPath.empty()?PlaybackState::Playing:PlaybackState::Loading;}
    else if(transport.state!=PlaybackState::Loading){
        const auto span=transport.bounds(editor.span,editor.total);
        const auto cursor=transport.cursor>=span.start&&transport.cursor<span.end?transport.cursor:span.start;
        beginAudition(audio,BoundaryCheck::None,cursor);
    }
}
void Workspace::seekTransport(Audio& audio,Frame frame){
    const auto span=transport.bounds(editor.span,editor.total);
    frame=std::clamp(frame,span.start,span.end-1);
    if(transport.engaged())beginAudition(audio,transport.boundary,frame,transport.state==PlaybackState::Paused);
    else{transport.range=span;transport.cursor=frame;}
}
void Workspace::playbackOverlay(){
    const auto bounds=ImPlot::GetPlotLimits();auto draw=ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    if(transport.engaged()&&transport.range!=editor.span){
        draw->AddRectFilled(ImPlot::PlotToPixels(static_cast<double>(transport.range.start),bounds.Y.Max),ImPlot::PlotToPixels(static_cast<double>(transport.range.end),bounds.Y.Min),IM_COL32(100,155,235,18));
    }
    const auto first=ImPlot::PlotToPixels(static_cast<double>(editor.span.start),bounds.Y.Max),last=ImPlot::PlotToPixels(static_cast<double>(editor.span.end),bounds.Y.Min);
    if(selectionDefined)draw->AddRectFilled(first,last,IM_COL32(70,210,158,28));
    const auto marker=ImPlot::PlotToPixels(static_cast<double>(transport.cursor),0);
    const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();
    draw->AddLine({marker.x,pos.y},{marker.x,pos.y+size.y},IM_COL32(245,245,245,255),2);
    ImPlot::PopPlotClipRect();
}
void Workspace::process(){
    if(!client)return;
    for(auto& reply:client->collect()){
        const auto& tag=reply.request.tag;
        if(tag!="libraries" && tag!="command" && tag!="collections" && reply.request.generation!=generation)continue;
        if((tag=="hash"||tag=="resolve")&&(!pendingMark||reply.request.body.at("mark")!=pendingMark->ticket))continue;
        if(tag=="source-import"&&(!pendingMark||reply.request.body.at("request_id")!=pendingMark->importRequest))continue;
        if(tag=="collections" && reply.request.path!=endpoint(library)+"/collections")continue;
        if(tag=="browse" && reply.request.path!=browsePath)continue;
        if(tag=="audition" && (reply.request.path!=auditionPath || reply.request.body.at("epoch")!=transportEpoch))continue;
        if(!reply.error.empty()){if(pendingMark&&(tag=="hash"||tag=="resolve"||tag=="source-import"||tag=="libraries"))pendingMark->failed=true;if(tag=="audition"){auditionPath.clear();audition.stop();transport.state=PlaybackState::Error;}notice=reply.error;if(reply.status==409){notice="Conflict: local draft retained. Reload the saved sample or Fork before saving. "+notice;if(tag=="command")for(auto& row:pending)if(row.at("command").at("request_id")==reply.request.body.at("request_id")){row["conflict"]=true;if(row.contains("source"))sampleLibrary.conflict(SourceRef::parse(row.at("source")),row.at("draft_id"),row.at("command").at("request_id"));}persist();}continue;}
        if(tag=="libraries"){
            libraries=reply.body;notice="Connected";
            if(library.empty()&&!libraries.empty())library=libraries[0].at("id");
            browse();
            if(pendingMark)submitMark();
            else if(!capture.is_null()){loadDetails();fetchSourceRegions();}
            for(const auto& entry:pending)if(!entry.value("conflict",false))client->submit({"command","POST",endpoint(entry.at("library"))+"/commands",entry.at("command"),generation,{}});
        }else if(tag=="browse"){items=reply.body.at("items");browseTotal=reply.body.at("total");}
        else if(tag=="sourceRegions"){
            if(reply.body.at("source")!=sourceRef().json()||reply.body.at("offset")!=sourceRegionOffset)continue;
            sourceRegions=reply.body.at("items");sourceRegionTotal=reply.body.at("total");
        }
        else if(tag=="collectionMember")items.push_back(reply.body);
        else if(tag=="reloadRegion"){
            if(activeDraft.empty())continue;
            const auto& r=reply.body;auto value=draftValue();value.span={r.at("span").at("start_frame"),r.at("span").at("end_frame")};value.attributes["name"]=r.at("name");value.attributes["kind"]=r.at("kind");
            sampleLibrary.reloadSaved(sourceRef(),activeDraft,r,value);activateDraft(activeDraft);fetchSourceRegions();persist();
        }
        else if(tag=="collections")collections=reply.body.at("items");
        else if(tag=="overview"){
            overview=reply.body;overviewBins.clear();Frame at=overview.value("origin_frame",Frame{0});const Frame step=overview.at("frames_per_bin");
            for(const auto& p:overview.at("peaks")){overviewBins.push_back({at,std::min(editor.total,at+step),p[0].get<double>(),p[1].get<double>(),p[2].get<double>(),p[3].get<double>()});at+=step;}
            ++waveformRevision;
        }
        else if(tag=="pcm"){
            pcmStart=reply.body.at("origin_frame");pcm=std::move(reply.pcm);pcmEnd=pcmStart+static_cast<Frame>(pcm.size()/2);++waveformRevision;
        }else if(tag=="audition"){
            auditionPath.clear();++auditionChunks;const auto origin=reply.body.at("origin_frame").get<Frame>();
            if(reply.request.body.at("first").get<bool>()){
                const bool paused=transport.state==PlaybackState::Paused;
                try{
                    audition.start(std::move(reply.pcm),origin,transport.range,transport.cursor,transport.effectiveLoop(),paused);
                    audition.gain(previewGain);transport.state=paused?PlaybackState::Paused:PlaybackState::Playing;
                }catch(const std::exception& e){audition.stop();transport.state=PlaybackState::Error;notice=std::string("Playback: ")+e.what();continue;}
            }else audition.enqueue(std::move(reply.pcm),origin);
            notice="";
        }
        else if(tag=="timing"){
            timing=reply.body;anchors.clear();onsets.clear();gridId.clear();gridRevision=0;
            for(const auto& result:timing.value("results",Json::array())){
                const auto content=result.value("result",result);
                for(const auto& f:content.value("onsets",Json::array()))onsets.push_back(f.get<Frame>());
                for(const auto& a:content.value("selected_anchors",Json::array()))anchors.push_back(a);
            }
            for(const auto& proposal:timing.value("proposals",Json::array())){
                const auto& body=proposal.at("proposal");
                if(body.contains("onsets")){onsets.clear();for(const auto& f:body.at("onsets"))onsets.push_back(f.get<Frame>());}
                if(body.contains("anchors")){anchors.clear();for(const auto& a:body.at("anchors"))anchors.push_back(a);}
            }
            for(const auto& grid:timing.value("grids",Json::array())){anchors.clear();for(const auto& a:grid.at("anchors"))anchors.push_back(a);gridId=grid.at("id");gridRevision=grid.at("revision");}
            beats.clear();for(const auto& a:anchors)beats.push_back(a.at("frame").get<Frame>());
            selectedAnchor=0;
        }else if(tag=="hash"){
            const auto digest=reply.body.at("sha256").get<std::string>();
            if(!pendingMark->digest.empty()&&pendingMark->digest!=digest){
                pendingMark->failed=true;notice="This file changed after the selection was captured. Reload the song before editing.";persist();continue;
            }
            pendingMark->digest=digest;persist();
            if(!preparation.match.is_null()&&preparation.digest==reply.body.at("sha256").get<std::string>()){finishMark(preparation.match);continue;}
            client->submit({"resolve","GET","/v1/resolve?sha256="+reply.body.at("sha256").get<std::string>(),{{"mark",pendingMark->ticket}},generation,{}});
        }else if(tag=="resolve"){
            const auto matches=reply.body.at("matches");
            if(matches.size()==1)finishMark(matches[0]);
            else if(matches.empty())importMarkedSource();
            else{pendingMark->failed=true;markMatches=matches;notice="Several recordings match this file. Choose its source below.";}
        }else if(tag=="source-import"){
            if(reply.body.at("capture").at("blob_sha256")!=pendingMark->digest){
                pendingMark->failed=true;notice="Registered source does not match the selected file. Your range is retained.";persist();continue;
            }
            finishMark(reply.body);
        }else if(tag=="capture"){
            const auto savedRegion=region;openCapture(reply.body);
            if(!savedRegion.is_null())chooseRegion(savedRegion);
        }else if(tag=="sampleMetadata"){
            if(reply.request.body!=draftSnapshot()){notice="Metadata response retained on server; newer local edits preserved.";continue;}
            const bool clean=!activeDraft.empty()&&!sampleLibrary.draft(sourceRef(),activeDraft).dirty();
            metadata=reply.body;metadataTarget=metadata.at("target_id");metadataTargetRevision=metadata.at("target_revision");metadataRevision=metadata.at("revision");metadataFields(metadata.at("effective"));
            stash();if(clean){auto& d=sampleLibrary.draft(sourceRef(),activeDraft);d.baseline=d.value;}
        }else if(tag=="export"){
            const auto& v=reply.body.at("variant");const auto filename=v.at("id").get<std::string>()+fs::path(v.at("location").get<std::string>()).extension().string();
            client->submit({"artifact","GET",reply.body.at("download_url"),{{"expected_sha256",v.at("audio_sha256")}},generation,directory/"exports"/filename});
        }else if(tag=="job"){
            const auto state=reply.body.at("state").get<std::string>();
            if(state=="succeeded"){analysisJob.clear();notice="Analysis ready: estimates are proposals.";loadDetails(true);if(editedFacets.empty()&&!metadataTarget.empty())get("sampleMetadata",endpoint(library)+"/metadata/"+metadataTarget+"?revision="+std::to_string(metadataTargetRevision));}
            else if(state=="failed"||state=="cancelled"){analysisJob.clear();notice="Analysis "+state+": "+reply.body.value("error",std::string{});}
            else jobDue=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        }else if(tag=="artifact")notice="Exported: "+reply.body.at("path").get<std::string>();
        else if(tag=="command"){
            if(reply.request.body.at("operation")=="region.save"){
                const auto request=reply.request.body.at("request_id").get<std::string>();
                auto row=std::find_if(pending.begin(),pending.end(),[&](const auto& p){return p.at("command").at("request_id")==request;});
                if(row==pending.end())continue;
                stash();const auto source=SourceRef::parse(row->at("source"));const auto id=row->at("draft_id").get<std::string>();
                if(sampleLibrary.acknowledge(source,id,request,reply.body.at("region"))){
                    auto& d=sampleLibrary.draft(source,id);
                    const bool current=!capture.is_null()&&sourceRef()==source&&activeDraft==id;
                    if(current){
                        region=reply.body.at("region");editor.dirty=d.dirty();
                        if(reply.body.contains("metadata")&&!d.dirty()){
                            metadata=reply.body.at("metadata").at("metadata");metadataTarget=region.at("id");metadataTargetRevision=region.at("revision");metadataRevision=metadata.at("revision");
                            stash();d.baseline=d.value;
                        }
                        notice=d.dirty()?"Saved submitted revision; newer edits retained.":"Sample saved.";fetchSourceRegions();
                    }
                }
                pending.erase(row);persist();continue;
            }
            const auto id=reply.request.body.at("request_id");
            bool unchanged=false;for(const auto& row:pending)if(row.at("command").at("request_id")==id)unchanged=row.value("draft",Json())==draftSnapshot();
            pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& row){return row.at("command").at("request_id")==id;}),pending.end());
            if(reply.request.generation==generation && reply.request.path==endpoint(library)+"/commands"){
                if(reply.body.contains("region")){
                    const bool sampleScope=metadataTarget.empty() || (!region.is_null() && metadataTarget==region.at("id").get<std::string>());
                    region=reply.body.at("region");editor.dirty=!unchanged;
                    if(sampleScope && reply.body.contains("metadata")){
                        metadata=reply.body.at("metadata").at("metadata");metadataTarget=region.at("id");metadataTargetRevision=region.at("revision");metadataRevision=metadata.at("revision");
                    }
                    notice="Saved "+region.at("id").get<std::string>()+" revision "+std::to_string(region.at("revision").get<int>());
                    if(unchanged)get("sampleMetadata",endpoint(library)+"/metadata/"+region.at("id").get<std::string>()+"?revision="+std::to_string(region.at("revision").get<int>()));
                }
                else notice="Saved";
                if(reply.body.contains("job_id")){analysisJob=reply.body.at("job_id");get("job",endpoint(library)+"/jobs/"+analysisJob);notice="Analysis queued; editing remains available.";}
                if(reply.request.body.at("operation")=="classify" || reply.request.body.at("operation")=="grid.edit" || reply.request.body.at("operation")=="grid.create")loadDetails(true);
                if(unchanged && reply.request.body.at("operation")=="metadata.write")get("sampleMetadata",endpoint(library)+"/metadata/"+metadataTarget+"?revision="+std::to_string(metadataTargetRevision));
                browse();
            }
            persist();
        }
    }
}
void Workspace::tick(Audio& audio){
    try{
        if(storageBlocked)return;
        if(!open&&audio.loaded){
            Connection warm;warm.url=serverUrl;warm.credentialFile=stateio::pathFromText(tokenFile);warm.startWorkstation=serverUrl=="http://127.0.0.1:8765";
            preparation.tick(warm,notify,utf8(audio.current),audio.displayPosition(),audio.duration,audio.loadedSerial);
        }
        const auto previousState=transport.state;
        if(open&&(!hadOpen||(audio.active&&!audio.paused)))audio.setPaused(true);hadOpen=open;
        if(open && !connectionAttempted)connect();
        process();
        if(!open&&transport.engaged())stopTransport();
        if(transport.engaged() && transport.state!=PlaybackState::Loading && (transport.state!=PlaybackState::Paused||auditionPath.empty())){
            const auto state=audition.snapshot();transport.cursor=state.cursor;auditionSubmitted=state.submitted;
            if(state.finished){transport.state=PlaybackState::Stopped;transport.cursor=transport.range.end;auditionPath.clear();audition.stop();}
            else if(transport.state!=PlaybackState::Paused)transport.state=state.buffering?PlaybackState::Buffering:PlaybackState::Playing;
            if(state.needed>=0&&auditionPath.empty()&&!state.finished)requestAudition(state.needed,false);
        }
        if(!capture.is_null() && transport.engaged() && editor.span!=auditionSelection && transport.scope!=PlaybackScope::Recording && !ImGui::IsMouseDown(0))beginAudition(audio,transport.boundary,{},transport.state==PlaybackState::Paused);
        if(transport.follow&&animating()){
            const auto width=viewportEnd-viewportStart;
            if(transport.cursor<viewportStart||transport.cursor>viewportEnd){viewportStart=static_cast<double>(std::max<Frame>(0,transport.cursor-static_cast<Frame>(width*.1)));viewportEnd=std::min(static_cast<double>(editor.total),viewportStart+width);fit=true;detailDue=std::chrono::steady_clock::now();}
        }
        if(previousState!=transport.state&&notify)notify();
        const auto now=std::chrono::steady_clock::now();
        if(!analysisJob.empty()&&now>=jobDue){jobDue=now+std::chrono::seconds(1);get("job",endpoint(library)+"/jobs/"+analysisJob);}
        if(detailDue!=std::chrono::steady_clock::time_point{} && now>=detailDue){detailDue={};loadDetails();}
        if(draftDirty && now>=persistDue)persist();
    }catch(const std::exception& e){notice=e.what();}
}
void Workspace::waveform(Audio& audio,const std::function<void(const char*,bool)>& observe){
    if(capture.is_null())return;
    if(ImPlot::BeginPlot("Overview",ImVec2(-1,95),ImPlotFlags_NoLegend|ImPlotFlags_NoMenus)){
        ImPlot::SetupAxes(nullptr,nullptr,ImPlotAxisFlags_NoTickLabels,ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxesLimits(0,static_cast<double>(editor.total),-1,1,ImGuiCond_Always);
        if(!overview.is_null()){
            std::vector<double>x,lo,hi;const Frame step=overview.at("frames_per_bin");
            Frame at=overview.value("origin_frame",Frame{0});
            for(const auto& p:overview.at("peaks")){x.push_back(static_cast<double>(at));lo.push_back(std::min(p[0].get<float>(),p[2].get<float>()));hi.push_back(std::max(p[1].get<float>(),p[3].get<float>()));at+=step;}
            ImPlot::PlotShaded("Recording",x.data(),lo.data(),hi.data(),static_cast<int>(x.size()));
        }
        double a=static_cast<double>(editor.span.start),b=static_cast<double>(editor.span.end);
        if(selectionDefined&&ImPlot::DragLineX(91,&a,ImVec4(.2f,.9f,.6f,1),2)){editor.beginGesture();editor.change({static_cast<Frame>(std::llround(a)),editor.span.end});}
        if(selectionDefined&&ImPlot::DragLineX(92,&b,ImVec4(1,.7f,.2f,1),2)){editor.beginGesture();editor.change({editor.span.start,static_cast<Frame>(std::llround(b))});}
        if(ImPlot::IsPlotHovered()&&ImGui::IsMouseDoubleClicked(0)){const auto frame=ImPlot::GetPlotMousePos().x;viewportStart=std::max(0.,frame-Rate*2);viewportEnd=std::min(static_cast<double>(editor.total),frame+Rate*2);fit=true;detailDue=std::chrono::steady_clock::now();}
        playbackOverlay();
        ImPlot::EndPlot();
    }
    observe("sample-waveform-overview",false);
    const auto previousMode=amplitude.mode;const auto previousGain=amplitude.gain;
    const auto modeButton=[&](const char* label,const char* id,AmplitudeMode mode){
        if(amplitude.mode==mode)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked=ImGui::Button(label);observe(id,clicked);
        if(amplitude.mode==mode)ImGui::PopStyleColor();
        return clicked;
    };
    ImGui::TextUnformatted("Display");ImGui::SameLine();
    if(modeButton("Actual level","sample-amplitude-actual",AmplitudeMode::Actual)){amplitude.actual();fitAmplitudeNow=false;}ImGui::SameLine();
    if(modeButton("Fit visible","sample-amplitude-fit",AmplitudeMode::Visible)){amplitude.mode=AmplitudeMode::Visible;fitAmplitudeNow=true;}ImGui::SameLine();
    if(modeButton("Manual","sample-amplitude-manual",AmplitudeMode::Manual)){amplitude.mode=AmplitudeMode::Manual;fitAmplitudeNow=false;}ImGui::SameLine();
    const bool fitNow=ImGui::Button("Fit now");observe("sample-amplitude-fit-now",fitNow);
    if(fitNow){amplitude.mode=AmplitudeMode::Manual;fitAmplitudeNow=true;}ImGui::SameLine();
    ImGui::Text("%.2fx / %+.1f dB",amplitude.gain,amplitude.db());
    if(!envelopePcm){ImGui::SameLine();ImGui::TextDisabled(viewportEnd-viewportStart>Rate*60?"Overview - zoom for detail":"Loading detailed waveform...");}
    const float height=std::max(180.f,ImGui::GetContentRegionAvail().y-360);
    const auto rulerPos=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##amplitude-ruler",{44,height});observe("sample-amplitude-ruler",false);
    if(ImGui::IsItemActivated())rulerInitial=amplitude.gain;
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0)){amplitude.drag(rulerInitial,-ImGui::GetMouseDragDelta(0).y);fitAmplitudeNow=false;}
    if(ImGui::IsItemHovered()){
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::SetTooltip("Display amplitude only; ruler labels are dBFS\nDrag up to enlarge, down to shrink\nDouble-click: actual level");
        if(ImGui::IsMouseDoubleClicked(0)){amplitude.actual();fitAmplitudeNow=false;}
    }
    ImGui::SameLine(0,0);
    if(ImPlot::BeginPlot("Stereo detail",ImVec2(-1,height),ImPlotFlags_NoLegend|ImPlotFlags_NoMenus)){
        ImPlot::SetupAxes("Time","L / R",ImPlotAxisFlags_None,ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisFormat(ImAxis_X1,timeLabel);
        ImPlot::SetupAxisLimits(ImAxis_Y1,-2.1,2.1,ImGuiCond_Always);
        if(fit){ImPlot::SetupAxisLimits(ImAxis_X1,viewportStart,viewportEnd,ImGuiCond_Always);fit=false;}
        const auto limits=ImPlot::GetPlotLimits();
        const double left=std::clamp(limits.X.Min,0.,static_cast<double>(editor.total-1)),right=std::clamp(limits.X.Max,left+1,static_cast<double>(editor.total));
        const auto range=waveformRange(left,right,editor.total);
        if(range!=waveformRange(viewportStart,viewportEnd,editor.total))detailDue=std::chrono::steady_clock::now()+std::chrono::milliseconds(180);
        viewportStart=left;viewportEnd=right;
        const auto first=range.start,end=range.end;
        const auto budget=static_cast<size_t>(std::clamp(ImPlot::GetPlotSize().x,64.f,8192.f));
        const auto now=std::chrono::steady_clock::now();
        const bool settled=detailDue==std::chrono::steady_clock::time_point{} || now>=detailDue;
        const bool changed=envelopeRevision!=waveformRevision || envelopeStart!=first || envelopeEnd!=end || envelopeBudget!=budget;
        if(changed && (settled || envelope.x.empty() || fitAmplitudeNow)){
            envelopePcm=envelope.fromPcm(pcm,pcmStart,first,end,budget);
            if(!envelopePcm)envelope.fromOverview(overviewBins,first,end);
            envelopeRevision=waveformRevision;envelopeStart=first;envelopeEnd=end;envelopeBudget=budget;++envelopeBuilds;projectedGain=-1;
            if(amplitude.mode==AmplitudeMode::Visible && !envelope.x.empty())amplitude.fit(envelope.peak,true);
        }
        if(fitAmplitudeNow && !envelope.x.empty()){
            amplitude.fit(envelope.peak,amplitude.mode==AmplitudeMode::Visible);fitAmplitudeNow=false;
        }
        if(projectedGain!=amplitude.gain){envelope.project(amplitude,displayEnvelope);projectedGain=amplitude.gain;}
        plotStereo("Detail",displayEnvelope.x,displayEnvelope.lmin,displayEnvelope.lmax,displayEnvelope.rmin,displayEnvelope.rmax,displayEnvelope.individual);
        // Draw a channel-aware amplitude ruler outside the plot's horizontal pan/zoom area.
        auto draw=ImGui::GetWindowDrawList();draw->AddRectFilled(rulerPos,{rulerPos.x+42,rulerPos.y+height},IM_COL32(29,34,38,255));
        for(const bool rightChannel:{false,true}){
            const auto zero=ImPlot::PlotToPixels(left,rightChannel?-1.:1.);
            draw->AddText({rulerPos.x+3,zero.y-8},IM_COL32(195,200,205,255),rightChannel?"R":"L");
            for(const double position:{-.8,-.4,.4,.8}){
                const auto pixel=ImPlot::PlotToPixels(left,(rightChannel?-1.:1.)+position);
                const auto level=std::abs(position)/(.9*amplitude.gain);char text[24]{};std::snprintf(text,sizeof(text),"%.0f",20*std::log10(level));
                draw->AddText({rulerPos.x+17,pixel.y-7},IM_COL32(165,175,185,255),text);
            }
        }
        for(const auto frame:beats)if(frame>=left&&frame<=right){double x=static_cast<double>(frame);ImPlot::DragLineX(static_cast<int>(frame%100000000)+100,&x,ImVec4(.4f,.5f,.7f,.35f),1,ImPlotDragToolFlags_NoInputs);}
        double a=static_cast<double>(editor.span.start),b=static_cast<double>(editor.span.end);
        const auto snapped=[&](double value){
            Frame f=static_cast<Frame>(std::llround(value));
            if(ImGui::GetIO().KeyAlt||snapMode==0)return f;
            if(snapMode==4 && !pcm.empty()){std::vector<Frame>crossings;const Frame first=std::max(pcmStart,f-200),last=std::min(pcmEnd-1,f+200);for(Frame i=first;i<last;++i){const auto j=static_cast<size_t>(i-pcmStart)*2;if(pcm[j]*pcm[j+2]<=0)crossings.push_back(i);}return editor.snap(f,crossings,200);}
            if(snapMode==3){std::vector<Frame> divisions;for(size_t i=1;i<beats.size();++i)for(Frame n=0;n<4;++n)divisions.push_back(beats[i-1]+(beats[i]-beats[i-1])*n/4);return editor.snap(f,divisions,static_cast<Frame>((right-left)*.02));}
            return editor.snap(f,snapMode==1?onsets:beats,static_cast<Frame>((right-left)*.02));
        };
        bool heldIn=false,heldOut=false,hoverIn=false,hoverOut=false;
        if(selectionDefined&&ImPlot::DragLineX(1,&a,ImVec4(.2f,1,.6f,1),3,0,nullptr,&hoverIn,&heldIn)){editor.beginGesture();editor.change({snapped(a),editor.span.end});}
        if(selectionDefined&&ImPlot::DragLineX(2,&b,ImVec4(1,.7f,.2f,1),3,0,nullptr,&hoverOut,&heldOut)){editor.beginGesture();editor.change({editor.span.start,snapped(b)});}
        if(heldIn||heldOut)boundaryHeld=true;
        const auto drag=ImGui::GetMouseDragDelta(0);
        if(ImPlot::IsPlotHovered()&&ImGui::IsMouseReleased(0)&&!boundaryHeld&&!hoverIn&&!hoverOut&&drag.x*drag.x+drag.y*drag.y<9)seekTransport(audio,static_cast<Frame>(std::llround(ImPlot::GetPlotMousePos().x)));
        if(ImGui::IsMouseReleased(0))boundaryHeld=false;
        playbackOverlay();
        ImPlot::EndPlot();
    }
    observe("sample-waveform-detail",false);
    if(previousMode!=amplitude.mode || previousGain!=amplitude.gain){draftDirty=true;persistDue=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);}
}
void Workspace::draw(Audio& audio,const std::function<void(const char*,bool)>& observe){
    if(!open)return;
    const auto size=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(size);
    if(!ImGui::Begin("Sampling",&open,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings)){ImGui::End();return;}
    const auto button=[&](const char* label,const char* id){bool pressed=ImGui::Button(label);observe(id,pressed);return pressed;};
    if(storageBlocked){
        ImGui::TextWrapped("Sample workspace state could not be read: %s. The file has been preserved. Restore sample-workspaces.json from its .bak copy or repair it, then Retry.",storageError.c_str());
        if(ImGui::Button("Retry reading workspace")){try{restore();storageBlocked=false;storageError.clear();}catch(const std::exception& e){storageError=e.what();}}
        if(ImGui::Button("Return to player"))open=false;
        ImGui::End();return;
    }
    try {
        if(button("Return to song","sample-return")){handoff.cancel();pendingMark.reset();markMatches=Json::array();persist();stopTransport();open=false;}
        ImGui::SameLine();if(button("Mark playing song","sample-mark"))mark(audio);
        ImGui::SameLine();if(button("Connection","sample-connection"))ImGui::OpenPopup("Backend connection");
        ImGui::SameLine();ImGui::TextUnformatted(notice.c_str());
        if(ImGui::BeginPopup("Backend connection")){input("Server URL",serverUrl);input("Token file",tokenFile);if(ImGui::Button("Connect")){connect();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
        if(!client || libraries.empty()){ImGui::TextWrapped("Prappy starts the local Workstation backend automatically. If startup failed, retry or inspect Connection.");if(button("Retry connection","sample-connect"))connect();}
        if(button("Songs in editor","sample-workspaces"))ImGui::OpenPopup("Editor songs");
        if(ImGui::BeginPopup("Editor songs")){
            for(const auto& [source,ws]:sampleLibrary.all()){
                if(!ws.view.contains("capture"))continue;
                const auto item=ws.view.at("capture");
                const auto label=item.at("recording").value("title",source.capture)+"##"+source.library+source.capture+source.timeline;
                const bool hit=ImGui::Selectable(label.c_str(),!capture.is_null()&&sourceRef()==source);observe(("sample-workspace-"+source.capture).c_str(),hit);
                if(hit){persist();library=source.library;openCapture(item);ImGui::CloseCurrentPopup();break;}
            }
            ImGui::EndPopup();
        }
        std::string libraryLabel="Choose library";for(const auto& lib:libraries)if(lib.at("id")==library)libraryLabel=lib.at("name");
        if(ImGui::BeginCombo("Library",libraryLabel.c_str())){for(const auto& lib:libraries){auto id=lib.at("id").get<std::string>();if(ImGui::Selectable((lib.at("name").get<std::string>()+"##"+id).c_str(),id==library)){persist();library=id;++generation;activeDraft.clear();capture=nullptr;region=nullptr;items=Json::array();browse();}}ImGui::EndCombo();}
        const bool sidebar=size.x>=780;
        if(sidebar){ImGui::BeginChild("Sample browser",{250,0},ImGuiChildFlags_Borders);}
        if(!capture.is_null()){
            ImGui::TextWrapped("Samples from this song");
            ImGui::BeginChild("Source samples",{0,190},ImGuiChildFlags_Borders);
            Json rows=sourceRegions;const auto& ws=sampleLibrary.at(sourceRef());
            for(const auto& [id,d]:ws.drafts){
                if(d.region)rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& r){return r.at("id")==d.region->id;}),rows.end());
                rows.push_back({{"id",id},{"local",id},{"name",d.value.attributes.value("name",std::string("Sample"))},
                    {"span",{{"start_frame",d.value.span.start},{"end_frame",d.value.span.end}}},
                    {"state",d.saving?(d.saving->conflict?"Conflict":"Saving / pending"):d.dirty()?"Draft saved locally":"Saved"}});
            }
            std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return std::make_tuple(a.at("span").at("start_frame").template get<Frame>(),a.at("id").template get<std::string>())<std::make_tuple(b.at("span").at("start_frame").template get<Frame>(),b.at("id").template get<std::string>());});
            size_t i=0;
            for(const auto& row:rows){
                const auto label=row.at("name").get<std::string>()+" ("+row.value("state",std::string("Saved"))+")##source-"+row.at("id").get<std::string>();
                const bool hit=ImGui::Selectable(label.c_str(),row.value("local",std::string{})==activeDraft);observe(("sample-source-item-"+std::to_string(i++)).c_str(),hit);
                if(hit){stash();if(row.contains("local")){transport.scope=PlaybackScope::Selection;activateDraft(row.at("local"));}else chooseRegion(row);break;}
            }
            if(rows.empty())ImGui::TextWrapped("No samples yet. Select a passage or create a sample at the cue.");
            ImGui::EndChild();
            if(sourceRegionOffset&&ImGui::Button("Earlier samples"))fetchSourceRegions(sourceRegionOffset>=100?sourceRegionOffset-100:0);
            if(sourceRegionOffset+sourceRegions.size()<sourceRegionTotal&&ImGui::Button("More samples"))fetchSourceRegions(sourceRegionOffset+100);
            if(button("New sample at cue","sample-new")){const auto start=std::clamp<Frame>(transport.cursor,0,std::max<Frame>(0,editor.total-std::min<Frame>(Rate*4,editor.total)));newDraft({start,std::min(editor.total,start+Rate*4)},true);}
            if(!activeDraft.empty()&&!sampleLibrary.draft(sourceRef(),activeDraft).region&&!sampleLibrary.draft(sourceRef(),activeDraft).saving){
                if(button("Discard this draft","sample-discard")){sampleLibrary.discard(sourceRef(),activeDraft);activeDraft.clear();selectionDefined=false;region=nullptr;persist();}
            }
            ImGui::Separator();
        }
        const char* tabs[]={"Recordings","Samples","Collections"};
        if(ImGui::Combo("Browse",&tab,tabs,3)){browseOffset=0;browse();draftDirty=true;}
        if(input("Search",libraryQuery)){browseOffset=0;browse();draftDirty=true;}
        if(ImGui::TreeNode("Metadata filter")){input("Facet",facet);input("Contains",facetValue);if(ImGui::Button("Apply filter")){browseOffset=0;browse();draftDirty=true;}ImGui::TreePop();}
        if(ImGui::Button("Refresh"))browse();
        if(tab!=2){ImGui::Text("%zu of %zu",items.size(),browseTotal);if(browseOffset>0 && ImGui::Button("Previous page")){browseOffset=browseOffset>=100?browseOffset-100:0;browse();}if(browseOffset+items.size()<browseTotal && ImGui::Button("Next page")){browseOffset+=100;browse();}}
        size_t itemIndex=0;
        for(const auto& item:items){
            const std::string id=item.at("id");std::string label=item.value("name",item.contains("recording")?item.at("recording").value("title",id):id);
            if(item.contains("metadata"))label=item.at("metadata").at("effective").value("title",label);
            const bool chosen=ImGui::Selectable((label+"##"+id).c_str());observe(("sample-item-"+std::to_string(itemIndex++)).c_str(),chosen);
            if(chosen){
                handoff.cancel();pendingMark.reset();markMatches=Json::array();persist();stopTransport();
                if(tab==0){openCapture(item);}
                else if(tab==1){transport.scope=PlaybackScope::Selection;++generation;region=item;get("capture",endpoint(library)+"/entities/"+item.at("capture_id").get<std::string>());}
                else{const auto members=item.at("members");items=Json::array();tab=1;for(const auto& member:members)get("collectionMember",endpoint(library)+"/entities/"+member.at("region").at("id").get<std::string>()+"?revision="+std::to_string(member.at("region").at("revision").get<int>()));break;}
            }
        }
        if(sidebar){ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("Sample editing",{0,0});}
        if(pendingMark){
            ImGui::TextWrapped("%s",notice.c_str());
            if(pendingMark->failed){
                for(const auto& match:markMatches){
                    const auto id=match.at("library_id").get<std::string>();std::string name=id;
                    for(const auto& lib:libraries)if(lib.at("id")==id)name=lib.at("name");
                    const auto label=match.at("capture").at("recording").value("title",std::string("Recording"))+" ("+name+" / "+match.at("capture").at("id").get<std::string>().substr(0,12)+")##"+match.at("capture").at("id").get<std::string>();
                    if(ImGui::Button(label.c_str())){const auto chosen=match;finishMark(chosen);break;}
                }
                if(ImGui::Button("Retry opening playing song")){pendingMark->failed=false;pendingMark->submitted=false;if(libraries.empty())connect();else submitMark();}
                if(ImGui::Button("Return to previous draft")){handoff.cancel();pendingMark.reset();markMatches=Json::array();notice="Previous draft restored.";loadDetails();persist();}
            }
        }else if(!capture.is_null()){
            ImGui::TextWrapped("%s",capture.at("recording").value("title","Recording").c_str());
            if(button("Play recording","sample-listen")){transport.scope=PlaybackScope::Recording;beginAudition(audio,BoundaryCheck::None,transport.cursor);}
            ImGui::SameLine();if(button("Classify / detect beats","sample-classify")){Json payload={{"capture_id",capture.at("id")},{"start_frame",std::max<Frame>(0,editor.span.start-Rate*10)},{"end_frame",std::min(editor.total,editor.span.end+Rate*10)}};if(!region.is_null()){payload["target_id"]=region.at("id");payload["target_revision"]=region.at("revision");}command("classify",payload);}
            if(!selectionDefined)ImGui::TextWrapped("No sample selected. Create a sample at the cue or select a passage on the player waveform.");
            ImGui::BeginDisabled(!selectionDefined);
            const bool zoomSelection=button("Zoom to selection (F)","sample-fit");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Fill the time view with the selected sample and 10%% padding on each side. Display only.");
            if(zoomSelection||(ImGui::Shortcut(ImGuiKey_F)&&!ImGui::GetIO().WantTextInput)){
                const auto margin=std::max<Frame>(100,(editor.span.end-editor.span.start)/10);
                viewportStart=static_cast<double>(std::max<Frame>(0,editor.span.start-margin));
                viewportEnd=static_cast<double>(std::min(editor.total,editor.span.end+margin));
                fit=true;detailDue=std::chrono::steady_clock::now();
            }
            ImGui::EndDisabled();
            waveform(audio,observe);if(!ImGui::IsMouseDown(0))editor.endGesture();
            ImGui::BeginDisabled(!selectionDefined);
            Span edited=editor.span;ImGui::SetNextItemWidth(150);ImGui::InputScalar("In",ImGuiDataType_S64,&edited.start);ImGui::SameLine();ImGui::SetNextItemWidth(150);ImGui::InputScalar("Out (exclusive)",ImGuiDataType_S64,&edited.end);editor.change(edited);
            ImGui::Text("Length: %lld frames / %.6f seconds",static_cast<long long>(editor.span.end-editor.span.start),static_cast<double>(editor.span.end-editor.span.start)/Rate);
            if(button("In -1","sample-in-minus"))editor.change({editor.span.start-1,editor.span.end});ImGui::SameLine();if(button("In +1","sample-in-plus"))editor.change({editor.span.start+1,editor.span.end});ImGui::SameLine();
            if(button("Out -1","sample-out-minus"))editor.change({editor.span.start,editor.span.end-1});ImGui::SameLine();if(button("Out +1","sample-out-plus"))editor.change({editor.span.start,editor.span.end+1});ImGui::SameLine();
            if(button("Undo","sample-undo"))editor.undo();ImGui::SameLine();if(button("Redo","sample-redo"))editor.redo();ImGui::SameLine();
            if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z))editor.undo();if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y))editor.redo();
            ImGui::SetNextItemWidth(160);const char* modes[]={"Snap off","Onsets","Beats","Sixteenth notes","Zero crossing"};ImGui::Combo("Snap",&snapMode,modes,5);ImGui::SameLine();ImGui::TextDisabled("Alt bypasses snap");
            const auto chooseScope=[&](const char* label,const char* id,PlaybackScope value){
                const bool selected=transport.scope==value;if(selected)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                const bool clicked=button(label,id);if(selected)ImGui::PopStyleColor();
                if(clicked){const bool active=transport.engaged(),paused=transport.state==PlaybackState::Paused;const auto cursor=transport.cursor;stopTransport();transport.scope=value;transport.range=transport.bounds(editor.span,editor.total);transport.cursor=value==PlaybackScope::Recording?std::clamp(cursor,transport.range.start,transport.range.end-1):transport.range.start;if(active)beginAudition(audio,BoundaryCheck::None,transport.cursor,paused);draftDirty=true;}
            };
            chooseScope("Selection","sample-scope-selection",PlaybackScope::Selection);ImGui::SameLine();
            chooseScope("Context","sample-scope-context",PlaybackScope::Context);ImGui::SameLine();
            chooseScope("Recording","sample-scope-recording",PlaybackScope::Recording);ImGui::SameLine();
            const bool loopChanged=ImGui::Checkbox("Loop",&transport.loop);
            if(loopChanged){
                if(transport.engaged()){
                    if(transport.state==PlaybackState::Loading||(transport.state==PlaybackState::Paused&&!auditionPath.empty()))beginAudition(audio,transport.boundary,transport.cursor,transport.state==PlaybackState::Paused);
                    else{audition.loop(transport.effectiveLoop());++transportEpoch;auditionPath.clear();}
                }
                draftDirty=true;
            }observe("sample-loop",loopChanged);ImGui::SameLine();
            const bool followChanged=ImGui::Checkbox("Follow playhead",&transport.follow);if(followChanged)draftDirty=true;observe("sample-follow",followChanged);
            const char* playLabel=transport.state==PlaybackState::Paused?"Resume":transport.state==PlaybackState::Loading?"Loading...":animating()?"Pause":"Play";
            if(button(playLabel,"sample-play"))toggleTransport(audio);ImGui::SameLine();
            if(button("Stop","sample-stop")){stopTransport();transport.range=transport.bounds(editor.span,editor.total);transport.cursor=transport.range.start;}ImGui::SameLine();
            if(button("Hear start","sample-start"))beginAudition(audio,BoundaryCheck::Start);ImGui::SameLine();
            if(button("Hear end","sample-end"))beginAudition(audio,BoundaryCheck::End);ImGui::SameLine();
            ImGui::SetNextItemWidth(100);if(ImGui::SliderFloat("Preview gain",&previewGain,0,1))audition.gain(previewGain);
            ImGui::Text("%s %s%s | %.3f / %.3f s | source %.3f s",transport.status(),transport.label(),transport.effectiveLoop()?" (Loop)":"",static_cast<double>(transport.cursor-transport.range.start)/Rate,static_cast<double>(transport.range.end-transport.range.start)/Rate,static_cast<double>(transport.cursor)/Rate);
            if(transport.engaged()&&editor.span!=auditionSelection)ImGui::TextDisabled("Boundary edits apply to audition on release");
            if(ImGui::Shortcut(ImGuiKey_Space)&&!ImGui::GetIO().WantTextInput)toggleTransport(audio);
            if(input("Sample name",regionName))draftDirty=true;
            const char* kinds[]={"groove","one_shot","pitched_articulation","ensemble_phrase","sustain","texture","transition","unknown"};
            if(ImGui::BeginCombo("Kind",kind.c_str())){for(const char* value:kinds)if(ImGui::Selectable(value,kind==value)){kind=value;draftDirty=true;}ImGui::EndCombo();}
            if(button("Save sample","sample-save")){
                Json payload={{"capture_id",capture.at("id")},{"name",regionName},{"kind",kind},{"span",{{"start_frame",editor.span.start},{"end_frame",editor.span.end}}},{"facts",humanFacts(true)}};
                if(!region.is_null()){payload["id"]=region.at("id");payload["expected"]=region.at("revision");}
                command("region.save",payload);
            }
            ImGui::SameLine();if(button("Save as new sample","sample-fork")){newDraft(editor.span,true);notice="Independent sample draft";}
            ImGui::SameLine();if(button("Reload saved","sample-reload")&&!region.is_null()){discardConflicts();get("reloadRegion",endpoint(library)+"/entities/"+region.at("id").get<std::string>());}
            ImGui::SameLine();if(button("Export sample","sample-export")&&!region.is_null())client->submit({"export","POST",endpoint(library)+"/regions/"+region.at("id").get<std::string>()+"/exports/"+std::to_string(region.at("revision").get<int>()),Json::object(),generation,{}});
            if(!region.is_null()){
                if(ImGui::BeginCombo("Add to collection","Choose collection")){for(const auto& col:collections){if(ImGui::Selectable(col.at("name").get<std::string>().c_str()))command("collection.add",{{"id",col.at("id")},{"expected",col.at("revision")},{"region",{{"id",region.at("id")},{"revision",region.at("revision")}}}});}ImGui::EndCombo();}
            }
            if(ImGui::TreeNode("New collection")){input("Collection name",collectionName);if(ImGui::Button("Create collection"))command("collection.create",{{"name",collectionName}});ImGui::TreePop();}
            if(ImGui::TreeNode("Recording / sample metadata")){
                ImGui::TextWrapped("Human corrections are separate from imported tags and machine proposals.");
                if(ImGui::Button("Edit recording metadata"))get("sampleMetadata",endpoint(library)+"/metadata/"+capture.at("recording_id").get<std::string>());
                if(!region.is_null()){ImGui::SameLine();if(ImGui::Button("Edit sample metadata"))get("sampleMetadata",endpoint(library)+"/metadata/"+region.at("id").get<std::string>()+"?revision="+std::to_string(region.at("revision").get<int>()));}
                ImGui::Text("Editing: %s",metadataTarget.empty()?"New sample":metadataTarget.starts_with("rec_")?"Recording":"Saved sample revision");
                for(auto it=fields.begin();it!=fields.end();++it){auto text=it.value().get<std::string>();if(input(it.key().c_str(),text)){it.value()=text;editedFacets.insert(it.key());draftDirty=true;persistDue=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);}}
                if(metadataTarget.empty())ImGui::TextDisabled("Save the sample to attach its metadata, or select Edit recording metadata.");
                if(ImGui::Button("Save metadata corrections")&&!metadataTarget.empty())command("metadata.write",{{"target_id",metadataTarget},{"target_revision",metadataTargetRevision},{"expected",metadataRevision},{"facts",humanFacts()}});
                if(ImGui::TreeNode("Evidence")){ImGui::TextWrapped("%s",metadata.dump(2).c_str());ImGui::TreePop();}
                ImGui::TreePop();
            }
            if(ImGui::TreeNode("Beat grid corrections")){
                if(!anchors.empty()){
                    selectedAnchor=std::clamp(selectedAnchor,0,static_cast<int>(anchors.size()-1));ImGui::InputInt("Beat index",&selectedAnchor);selectedAnchor=std::clamp(selectedAnchor,0,static_cast<int>(anchors.size()-1));
                    Frame frame=anchors[static_cast<size_t>(selectedAnchor)].at("frame");if(ImGui::InputScalar("Beat frame",ImGuiDataType_S64,&frame))anchors[static_cast<size_t>(selectedAnchor)]["frame"]=std::clamp<Frame>(frame,0,editor.total-1);
                    bool down=anchors[static_cast<size_t>(selectedAnchor)].value("downbeat",false);if(ImGui::Checkbox("Downbeat",&down))anchors[static_cast<size_t>(selectedAnchor)]["downbeat"]=down;
                    if(ImGui::Button("Delete beat"))anchors.erase(anchors.begin()+selectedAnchor);
                }
                if(ImGui::Button("Insert beat at In"))anchors.push_back({{"frame",editor.span.start},{"origin","human"},{"downbeat",false}});
                if(ImGui::Button("Save corrected grid")){
                    std::sort(anchors.begin(),anchors.end(),[](const auto&a,const auto&b){return a.at("frame").template get<Frame>()<b.at("frame").template get<Frame>();});
                    Json values=Json::array();for(auto a:anchors){a["origin"]="human";values.push_back(a);}
                    if(gridId.empty())command("grid.create",{{"capture_id",capture.at("id")},{"anchors",values}});
                    else{Json operations=Json::array();for(const auto& a:timing.at("grids").back().at("anchors"))operations.push_back({{"op","delete"},{"anchor_id",a.at("id")}});for(const auto& a:values)operations.push_back({{"op","insert"},{"anchor",a}});command("grid.edit",{{"id",gridId},{"expected",gridRevision},{"operations",operations}});}
                }
                ImGui::TextWrapped("%s",timing.is_null()?"Timing not loaded":timing.value("coverage",Json::object()).dump().c_str());ImGui::TreePop();
            }
            ImGui::EndDisabled();
            if(editor.span!=persistedSpan){draftDirty=true;persistDue=std::chrono::steady_clock::now()+(ImGui::IsMouseDown(0)?std::chrono::milliseconds(250):std::chrono::milliseconds(0));}
        }else ImGui::TextWrapped("Choose a recording, reopen a sample, or mark the song currently playing.");
        if(sidebar)ImGui::EndChild();
        if(!pending.empty()){ImGui::Separator();ImGui::Text("%zu pending command(s) retained locally",pending.size());if(ImGui::Button("Retry pending"))for(const auto& entry:pending)if(!entry.value("conflict",false))client->submit({"command","POST",endpoint(entry.at("library"))+"/commands",entry.at("command"),generation,{}});ImGui::SameLine();if(ImGui::Button("Discard unsaved metadata commands")){pending.erase(std::remove_if(pending.begin(),pending.end(),[](const auto& row){return !row.contains("source");}),pending.end());persist();}}
    }catch(const std::exception& e){notice=e.what();}
    ImGui::End();
}
}

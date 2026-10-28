#include "sample_workspace.h"
#include "state_io.h"
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
    tokenFile=utf8(fs::path(L"\\\\wsl.localhost\\ubuntu26-dev\\home\\home\\.config\\audioharvest\\server.token"));restore();
}
Workspace::~Workspace(){try{persist();}catch(const std::exception& e){OutputDebugStringA(e.what());}}
void Workspace::get(std::string tag,std::string path){if(client){Json context=tag=="sampleMetadata"?draftSnapshot():Json();client->submit({std::move(tag),"GET",std::move(path),context,generation,{}});}}
void Workspace::connect(){
    connectionAttempted=true;
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
    ++generation;++waveformRevision;overviewBins.clear();envelopePcm=false;envelope.clear();displayEnvelope.clear();projectedGain=-1;analysisJob.clear();capture=std::move(item);pcm.clear();pcmStart=pcmEnd=0;beats.clear();onsets.clear();anchors.clear();overview=nullptr;timing=nullptr;gridId.clear();gridRevision=0;
    const Frame total=capture.at("timeline").at("frame_count").get<Frame>();
    editor.reset(total,selected.value_or(Span{0,std::min<Frame>(total,Rate*4)}));
    viewportStart=static_cast<double>(std::max<Frame>(0,editor.span.start-Rate));
    viewportEnd=static_cast<double>(std::min<Frame>(total,editor.span.end+Rate));
    fit=true;metadata=capture.value("metadata",Json::object());
    metadataTarget.clear();metadataTargetRevision=1;
    metadataRevision=0;metadataFields(metadata.value("effective",Json::object()));
    if(region.is_null()){regionName=capture.at("recording").value("title","Sample")+" sample";kind="unknown";}
    loadDetails();draftDirty=true;persistDue=std::chrono::steady_clock::now();
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
    if(operation=="region.save")for(const auto& row:pending)if(row.at("library")==library && row.at("command").at("operation")==operation){notice="A sample save is pending. Retry it, or resolve its conflict before saving again.";return;}
    pending.push_back({{"library",library},{"command",body},{"draft",draftSnapshot()}});
    persist();
    client->submit({"command","POST",endpoint(library)+"/commands",body,generation,{}});
    notice="Saving command...";
}
Json Workspace::draftSnapshot()const{
    return {{"span",{editor.span.start,editor.span.end}},{"name",regionName},{"kind",kind},{"fields",fields},{"edited_facets",editedFacets},{"metadata_target",metadataTarget}};
}
void Workspace::discardConflicts(){
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& row){return row.at("library")==library && row.value("conflict",false);}),pending.end());
}
void Workspace::persist(){
    Json draft={{"capture",capture},{"region",region},{"span",{{"start",editor.span.start},{"end",editor.span.end}}},{"name",regionName},{"kind",kind},
                {"fields",fields},{"edited_facets",editedFacets},{"pending",pending},{"library",library},{"open",open},{"metadata",metadata},{"metadata_target",metadataTarget},{"metadata_target_revision",metadataTargetRevision},{"metadata_revision",metadataRevision},
                {"search",libraryQuery},{"facet",facet},{"facet_value",facetValue},{"tab",tab},{"offset",browseOffset},{"viewport_start",viewportStart},{"viewport_end",viewportEnd},{"snap",snapMode},{"loop",loop},{"gain",previewGain},{"amplitude_mode",static_cast<int>(amplitude.mode)},{"amplitude_gain",amplitude.gain}};
    stateio::write(directory/"sample-draft.json",draft.dump(2));draftDirty=false;persistedSpan=editor.span;
}
void Workspace::restore(){
    try{auto config=Json::parse(stateio::read(directory/"sampling-connection.json"));serverUrl=config.at("url");tokenFile=config.at("token_file");library=config.value("library","");}catch(const std::exception&){}
    try{auto d=Json::parse(stateio::read(directory/"sample-draft.json"));capture=d.at("capture");region=d.at("region");regionName=d.at("name");kind=d.at("kind");fields=d.at("fields");editedFacets=d.at("edited_facets").get<std::set<std::string>>();pending=d.at("pending");library=d.at("library");open=d.value("open",false);metadata=d.value("metadata",Json::object());metadataTarget=d.value("metadata_target","");metadataTargetRevision=d.value("metadata_target_revision",1);metadataRevision=d.value("metadata_revision",0);
        libraryQuery=d.value("search","");facet=d.value("facet","");facetValue=d.value("facet_value","");tab=std::clamp(d.value("tab",0),0,2);browseOffset=d.value("offset",size_t{0});viewportStart=d.value("viewport_start",0.);viewportEnd=d.value("viewport_end",48000.);snapMode=std::clamp(d.value("snap",0),0,4);loop=d.value("loop",false);previewGain=std::clamp(d.value("gain",.7f),0.f,1.f);
        amplitude.mode=static_cast<AmplitudeMode>(std::clamp(d.value("amplitude_mode",0),0,2));amplitude.gain=AmplitudeScale::bounded(d.value("amplitude_gain",1.));if(amplitude.mode==AmplitudeMode::Actual)amplitude.actual();
        if(!capture.is_null())editor.reset(capture.at("timeline").at("frame_count"),{d.at("span").at("start"),d.at("span").at("end")});
    }catch(const std::exception&){capture=nullptr;region=nullptr;pending=Json::array();}
}
void Workspace::mark(Audio& audio){
    open=true;audio.setPaused(true);audition.stop();
    if(!client){connect();notice="Connect, then mark the playing recording or choose one from Recordings.";return;}
    if(!capture.is_null() && audio.current==playingPath && playingCapture==capture.at("id").get<std::string>()){region=nullptr;const auto frame=static_cast<Frame>(std::llround(audio.position*Rate));auto item=capture;openCapture(item,Span{frame,std::min(editor.total,frame+Rate*4)});}
    else if(!audio.current.empty())client->submit({"hash","GET",utf8(audio.current),nullptr,generation,{}});
    else browse();
}
void Workspace::requestAudition(Span range){
    if(capture.is_null()||!client)return;
    const auto start=std::clamp<Frame>(range.start,0,editor.total-1),end=std::clamp<Frame>(range.end,start+1,editor.total);
    if(end-start>Rate*600){notice="Audition selections are limited to ten minutes.";return;}
    auditionPath=endpoint(library)+"/captures/"+capture.at("id").get<std::string>()+"/pcm?start="+std::to_string(start)+"&end="+std::to_string(end)+"&timeline="+capture.at("timeline_id").get<std::string>();get("audition",auditionPath);
}
void Workspace::process(Audio& audio){
    if(!client)return;
    for(auto& reply:client->collect()){
        const auto& tag=reply.request.tag;
        if(tag!="libraries" && tag!="command" && tag!="collections" && reply.request.generation!=generation)continue;
        if(tag=="collections" && reply.request.path!=endpoint(library)+"/collections")continue;
        if(tag=="browse" && reply.request.path!=browsePath)continue;
        if(tag=="audition" && reply.request.path!=auditionPath)continue;
        if(!reply.error.empty()){notice=reply.error;if(reply.status==409){notice="Conflict: local draft retained. Reload the saved sample or Fork before saving. "+notice;if(tag=="command")for(auto& row:pending)if(row.at("command").at("request_id")==reply.request.body.at("request_id"))row["conflict"]=true;persist();}continue;}
        if(tag=="libraries"){
            libraries=reply.body;notice="Connected";
            if(library.empty()&&!libraries.empty())library=libraries[0].at("id");
            browse();
            if(!capture.is_null()){auto savedFields=fields;auto savedEdited=editedFacets;auto savedRegion=region;auto savedMeta=metadata;auto target=metadataTarget;auto targetRev=metadataTargetRevision;auto metaRev=metadataRevision;auto span=editor.span;auto item=capture;auto left=viewportStart,right=viewportEnd;openCapture(item,span);fields=savedFields;editedFacets=savedEdited;region=savedRegion;metadata=savedMeta;metadataTarget=target;metadataTargetRevision=targetRev;metadataRevision=metaRev;viewportStart=left;viewportEnd=right;fit=true;loadDetails();}
            for(const auto& entry:pending)client->submit({"command","POST",endpoint(entry.at("library"))+"/commands",entry.at("command"),generation,{}});
        }else if(tag=="browse"){items=reply.body.at("items");browseTotal=reply.body.at("total");}
        else if(tag=="collectionMember")items.push_back(reply.body);
        else if(tag=="reloadRegion"){region=reply.body;get("capture",endpoint(library)+"/entities/"+region.at("capture_id").get<std::string>());}
        else if(tag=="collections")collections=reply.body.at("items");
        else if(tag=="overview"){
            overview=reply.body;overviewBins.clear();Frame at=overview.value("origin_frame",Frame{0});const Frame step=overview.at("frames_per_bin");
            for(const auto& p:overview.at("peaks")){overviewBins.push_back({at,std::min(editor.total,at+step),p[0].get<double>(),p[1].get<double>(),p[2].get<double>(),p[3].get<double>()});at+=step;}
            ++waveformRevision;
        }
        else if(tag=="pcm"){
            pcmStart=reply.body.at("origin_frame");pcm=std::move(reply.pcm);pcmEnd=pcmStart+static_cast<Frame>(pcm.size()/2);++waveformRevision;
        }else if(tag=="audition"){audition.play(std::move(reply.pcm),loop);audition.gain(previewGain);notice="Auditioning canonical PCM";}
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
            get("resolve",endpoint(library)+"/resolve?sha256="+reply.body.at("sha256").get<std::string>());
        }else if(tag=="resolve"){
            const auto& matches=reply.body.at("matches");
            if(matches.size()==1){region=nullptr;const auto frame=static_cast<Frame>(std::llround(audio.position*Rate));openCapture(matches[0],Span{frame,frame+Rate*4});}
            else notice=matches.empty()?"This file is not registered in the selected library. Import it in audioharvest first.":"Multiple captures match. Choose the intended recording in the browser.";
        }else if(tag=="capture"){
            const auto savedRegion=region;openCapture(reply.body,savedRegion.is_null()?std::optional<Span>{}:std::optional<Span>{{savedRegion.at("span").at("start_frame"),savedRegion.at("span").at("end_frame")}});
            region=savedRegion;
            if(!region.is_null()){regionName=region.at("name");kind=region.at("kind");get("sampleMetadata",endpoint(library)+"/metadata/"+region.at("id").get<std::string>()+"?revision="+std::to_string(region.at("revision").get<int>()));}
        }else if(tag=="sampleMetadata"){
            if(reply.request.body!=draftSnapshot()){notice="Metadata response retained on server; newer local edits preserved.";continue;}
            metadata=reply.body;metadataTarget=metadata.at("target_id");metadataTargetRevision=metadata.at("target_revision");metadataRevision=metadata.at("revision");metadataFields(metadata.at("effective"));
        }else if(tag=="download"){
            playingPath=fs::path(std::u8string(reinterpret_cast<const char8_t*>(reply.body.at("path").get_ref<const std::string&>().data()),reply.body.at("path").get_ref<const std::string&>().size()));
            playingCapture=capture.at("id");audio.play(playingPath);notice="Listening to original";
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
        if(open && !connectionAttempted)connect();
        process(audio);
        const auto now=std::chrono::steady_clock::now();
        if(!analysisJob.empty()&&now>=jobDue){jobDue=now+std::chrono::seconds(1);get("job",endpoint(library)+"/jobs/"+analysisJob);}
        if(detailDue!=std::chrono::steady_clock::time_point{} && now>=detailDue){detailDue={};loadDetails();}
        if(draftDirty && now>=persistDue)persist();
    }catch(const std::exception& e){notice=e.what();}
}
void Workspace::waveform(const std::function<void(const char*,bool)>& observe){
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
        if(ImPlot::DragLineX(91,&a,ImVec4(.2f,.9f,.6f,1),2)){editor.beginGesture();editor.change({static_cast<Frame>(std::llround(a)),editor.span.end});}
        if(ImPlot::DragLineX(92,&b,ImVec4(1,.7f,.2f,1),2)){editor.beginGesture();editor.change({editor.span.start,static_cast<Frame>(std::llround(b))});}
        if(ImPlot::IsPlotHovered()&&ImGui::IsMouseDoubleClicked(0)){const auto frame=ImPlot::GetPlotMousePos().x;viewportStart=std::max(0.,frame-Rate*2);viewportEnd=std::min(static_cast<double>(editor.total),frame+Rate*2);fit=true;detailDue=std::chrono::steady_clock::now();}
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
    const float height=std::max(150.f,ImGui::GetContentRegionAvail().y-290);
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
        if(ImPlot::DragLineX(1,&a,ImVec4(.2f,1,.6f,1),3)){editor.beginGesture();editor.change({snapped(a),editor.span.end});}
        if(ImPlot::DragLineX(2,&b,ImVec4(1,.7f,.2f,1),3)){editor.beginGesture();editor.change({editor.span.start,snapped(b)});}
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
    try {
        if(button("Return to song","sample-return")){audition.stop();open=false;}
        ImGui::SameLine();if(button("Mark playing song","sample-mark"))mark(audio);
        ImGui::SameLine();if(button("Connection","sample-connection"))ImGui::OpenPopup("Backend connection");
        ImGui::SameLine();ImGui::TextUnformatted(notice.c_str());
        if(ImGui::BeginPopup("Backend connection")){input("Server URL",serverUrl);input("Token file",tokenFile);if(ImGui::Button("Connect")){connect();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
        if(!client || libraries.empty()){ImGui::TextWrapped("Prappy starts the local Workstation backend automatically. If startup failed, retry or inspect Connection.");if(button("Retry connection","sample-connect"))connect();}
        std::string libraryLabel="Choose library";for(const auto& lib:libraries)if(lib.at("id")==library)libraryLabel=lib.at("name");
        if(ImGui::BeginCombo("Library",libraryLabel.c_str())){for(const auto& lib:libraries){auto id=lib.at("id").get<std::string>();if(ImGui::Selectable((lib.at("name").get<std::string>()+"##"+id).c_str(),id==library)){library=id;++generation;capture=nullptr;region=nullptr;items=Json::array();browse();}}ImGui::EndCombo();}
        const bool sidebar=size.x>=780;
        if(sidebar){ImGui::BeginChild("Sample browser",{250,0},ImGuiChildFlags_Borders);}
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
                audition.stop();
                if(tab==0){region=nullptr;openCapture(item);}
                else if(tab==1){++generation;region=item;get("capture",endpoint(library)+"/entities/"+item.at("capture_id").get<std::string>());}
                else{const auto members=item.at("members");items=Json::array();tab=1;for(const auto& member:members)get("collectionMember",endpoint(library)+"/entities/"+member.at("region").at("id").get<std::string>()+"?revision="+std::to_string(member.at("region").at("revision").get<int>()));break;}
            }
        }
        if(sidebar){ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("Sample editing",{0,0});}
        if(!capture.is_null()){
            ImGui::TextWrapped("%s",capture.at("recording").value("title","Recording").c_str());
            if(button("Listen to recording","sample-listen")){
                audition.stop();const auto path=directory/"originals"/(capture.at("id").get<std::string>()+".audio");
                client->submit({"download","GET",capture.at("original_url"),{{"expected_sha256",capture.at("blob_sha256")}},generation,path});
            }
            ImGui::SameLine();if(button("Classify / detect beats","sample-classify")){Json payload={{"capture_id",capture.at("id")},{"start_frame",std::max<Frame>(0,editor.span.start-Rate*10)},{"end_frame",std::min(editor.total,editor.span.end+Rate*10)}};if(!region.is_null()){payload["target_id"]=region.at("id");payload["target_revision"]=region.at("revision");}command("classify",payload);}
            waveform(observe);if(!ImGui::IsMouseDown(0))editor.endGesture();
            Span edited=editor.span;ImGui::SetNextItemWidth(150);ImGui::InputScalar("In",ImGuiDataType_S64,&edited.start);ImGui::SameLine();ImGui::SetNextItemWidth(150);ImGui::InputScalar("Out (exclusive)",ImGuiDataType_S64,&edited.end);editor.change(edited);
            ImGui::Text("Length: %lld frames / %.6f seconds",static_cast<long long>(editor.span.end-editor.span.start),static_cast<double>(editor.span.end-editor.span.start)/Rate);
            if(button("In -1","sample-in-minus"))editor.change({editor.span.start-1,editor.span.end});ImGui::SameLine();if(button("In +1","sample-in-plus"))editor.change({editor.span.start+1,editor.span.end});ImGui::SameLine();
            if(button("Out -1","sample-out-minus"))editor.change({editor.span.start,editor.span.end-1});ImGui::SameLine();if(button("Out +1","sample-out-plus"))editor.change({editor.span.start,editor.span.end+1});ImGui::SameLine();
            if(button("Undo","sample-undo"))editor.undo();ImGui::SameLine();if(button("Redo","sample-redo"))editor.redo();ImGui::SameLine();
            if(button("Fit selection","sample-fit")){viewportStart=static_cast<double>(std::max<Frame>(0,editor.span.start-100));viewportEnd=static_cast<double>(std::min(editor.total,editor.span.end+100));fit=true;detailDue=std::chrono::steady_clock::now();}
            if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z))editor.undo();if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y))editor.redo();
            ImGui::SetNextItemWidth(160);const char* modes[]={"Snap off","Onsets","Beats","Sixteenth notes","Zero crossing"};ImGui::Combo("Snap",&snapMode,modes,5);ImGui::SameLine();ImGui::TextDisabled("Alt bypasses snap");
            if(button("Play selection","sample-play")){audio.setPaused(true);requestAudition(editor.span);}ImGui::SameLine();
            if(button("Hear start","sample-start")){audio.setPaused(true);requestAudition({editor.span.start-Rate/4,editor.span.start+Rate});}ImGui::SameLine();
            if(button("Hear end","sample-end")){audio.setPaused(true);requestAudition({editor.span.end-Rate,editor.span.end+Rate/4});}ImGui::SameLine();
            if(button("Stop","sample-stop")){auditionPath.clear();audition.stop();}ImGui::SameLine();ImGui::Checkbox("Loop",&loop);ImGui::SameLine();ImGui::SetNextItemWidth(100);if(ImGui::SliderFloat("Preview gain",&previewGain,0,1))audition.gain(previewGain);
            if(input("Sample name",regionName))draftDirty=true;
            const char* kinds[]={"groove","one_shot","pitched_articulation","ensemble_phrase","sustain","texture","transition","unknown"};
            if(ImGui::BeginCombo("Kind",kind.c_str())){for(const char* value:kinds)if(ImGui::Selectable(value,kind==value)){kind=value;draftDirty=true;}ImGui::EndCombo();}
            if(button("Save sample","sample-save")){
                Json payload={{"capture_id",capture.at("id")},{"name",regionName},{"kind",kind},{"span",{{"start_frame",editor.span.start},{"end_frame",editor.span.end}}},{"facts",humanFacts(true)}};
                if(!region.is_null()){payload["id"]=region.at("id");payload["expected"]=region.at("revision");}
                command("region.save",payload);
            }
            ImGui::SameLine();if(button("Fork","sample-fork")){discardConflicts();region=nullptr;++generation;metadataTarget.clear();metadataRevision=0;notice="Independent sample draft";draftDirty=true;}
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
            if(editor.span!=persistedSpan){draftDirty=true;persistDue=std::chrono::steady_clock::now();}
        }else ImGui::TextWrapped("Choose a recording, reopen a sample, or mark the song currently playing.");
        if(sidebar)ImGui::EndChild();
        if(!pending.empty()){ImGui::Separator();ImGui::Text("%zu pending command(s) retained locally",pending.size());if(ImGui::Button("Retry pending"))for(const auto& entry:pending)client->submit({"command","POST",endpoint(entry.at("library"))+"/commands",entry.at("command"),generation,{}});ImGui::SameLine();if(ImGui::Button("Discard pending commands")){pending=Json::array();persist();}}
    }catch(const std::exception& e){notice=e.what();}
    ImGui::End();
}
}

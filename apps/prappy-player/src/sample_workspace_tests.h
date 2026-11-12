#pragma once
#include "sample_workspace.h"
#include <ostream>
#include "imgui_internal.h"
namespace sampling {
struct WorkspaceReadinessTests {
    static void run(Workspace& workspace,Audio& audio){
        const auto require=[](bool ok){if(!ok)throw std::runtime_error("Editor audio readiness invariant failed.");};
        const auto originalCapture=workspace.capture;
        const auto originalSpan=workspace.editor.span;
        const auto originalCursor=workspace.transport.cursor;
        auto reject=[&]{
            require(!workspace.audioReady());
            workspace.toggleTransport(audio);
            workspace.beginAudition(audio,BoundaryCheck::Start);
            workspace.beginAudition(audio,BoundaryCheck::End);
            workspace.seekTransport(audio,originalSpan.start);
            require(!workspace.transport.engaged()&&workspace.editor.span==originalSpan&&workspace.capture==originalCapture);
        };
        reject(); // Restored draft before its descriptor arrives.
        workspace.descriptorPath=workspace.audioEndpoint();
        const Json valid={{"capture",originalCapture},{"timeline_id",originalCapture.at("timeline_id")},
            {"sample_rate",workspace.rate},{"frame_count",workspace.editor.total},{"channels",2},{"sample_format","float32"},{"source_key",std::string(64,'a')}};
        workspace.preparedManifest=valid;require(workspace.audioCoverage().has_value());reject(); // Descriptor alone is not a playback owner.
        for(const auto& field:{"timeline_id","sample_rate","frame_count"}){
            workspace.preparedManifest=valid;workspace.preparedManifest[field]=nullptr;require(!workspace.audioCoverage());reject();
        }
        workspace.preparedManifest=valid;workspace.preparedManifest["capture"]["id"]="wrong-source";require(!workspace.audioCoverage());reject();
        workspace.preparedManifest=valid;workspace.preparedManifest["coverage"]={{"start_frame",10},{"end_frame",5}};require(!workspace.audioCoverage());reject();
        workspace.preparedManifest=valid;workspace.descriptorPath+="/stale";require(!workspace.audioCoverage());reject();
        workspace.preparedManifest=valid;workspace.transport.state=PlaybackState::Paused;
        ++workspace.generation;workspace.selectAudioEndpoint(workspace.audioEndpoint());
        require(workspace.preparedManifest.is_null()&&!workspace.transport.engaged()&&workspace.contextManifest.is_null());
        Request stale{"capture","GET",workspace.descriptorPath,nullptr,workspace.generation-1,{}};
        require(!workspace.current(stale));reject();
        workspace.preparedManifest=nullptr;workspace.overviewDue=std::chrono::steady_clock::now();
        workspace.transport.state=PlaybackState::Paused;workspace.closeNativeSession();require(!workspace.transport.engaged());
        workspace.tick(audio);require(workspace.capture==originalCapture&&workspace.editor.span==originalSpan);
        // Completing descriptor acquisition creates an owner for precisely that
        // endpoint/key. No physical audio device or real backend is involved.
        auto ready=valid;ready["capture"]["blob_sha256"]=std::string(64,'b');
        ready["channels"]=2;ready["sample_format"]="float32";ready["access_mode"]="indexed";
        workspace.descriptorPath=workspace.audioEndpoint();workspace.preparedManifest=ready;
        Client::Transport offline=[](const Connection&,const Request& request,std::stop_token){Reply reply;reply.request=request;reply.error="Controlled unavailable PCM";return reply;};
        workspace.nativePlayback=std::make_shared<NativePlayback>(Connection{},workspace.pcmBudget,[]{},offline);
        workspace.nativePlayback->load(workspace.descriptorPath,originalCapture.at("id"),std::string(64,'b'),true,0,ready);
        require(workspace.audioReady());workspace.beginAudition(audio);require(workspace.transport.state==PlaybackState::Loading);
        const auto errorDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(workspace.nativePlayback->state().error.empty()&&std::chrono::steady_clock::now()<errorDeadline){workspace.nativePlayback->tick();SDL_Delay(1);}
        require(!workspace.nativePlayback->state().error.empty());
        require(workspace.audioReady()); // Transport failure does not invalidate a verified source.
        workspace.beginAudition(audio);require(workspace.nativePlayback->state().error.empty()&&workspace.transport.state==PlaybackState::Loading);
        workspace.preparedManifest["source_key"]=std::string(64,'c');reject(); // A different version cannot reuse this owner.
        workspace.closeNativeSession();workspace.preparedManifest=nullptr;
        workspace.transport.cursor=originalCursor;
    }
};
}
inline void sampleWorkspaceTests(const std::filesystem::path& root,std::ostream& log){
    using namespace sampling;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const SourceRef source{"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b'),"tl_"+std::string(32,'c')};
    const Frame rate=44100,total=rate*10;
    const auto directory=root/"restored-native-editor";std::filesystem::create_directories(directory);
    const Json capture={{"id",source.capture},{"blob_sha256",std::string(64,'b')},{"timeline_id",source.timeline},
        {"timeline",{{"frame_count",total},{"sample_rate",rate},{"channels",2},{"sample_format","float32"}}},{"recording",{{"title","Saved sample"}}}};
    SampleLibrary samples;auto& ws=samples.open(source,total);
    samples.select(source,{{rate*2+1000,rate*2+2000},{{"name","Saved draft"}}},"restored-draft");
    ws.view={{"capture",capture},{"viewport_start",rate*2},{"viewport_end",rate*3},{"cursor",rate*2+1500}};
    stateio::write(directory/"sample-workspaces.json",Json{{"workspaces",samples.json()},{"active_source",source.json()},
        {"prepared_manifest",nullptr},{"source_handoff",nullptr},{"commands",Json::array()},{"open",true},{"browser_library",source.library},
        {"search",""},{"facet",""},{"facet_value",""},{"tab",0},{"offset",0},{"gain",.7}}.dump());
    stateio::write(directory/"sampling-connection.json",Json{{"url","http://127.0.0.1:1"},{"token_file",utf8(directory/"absent-token")},{"library",source.library}}.dump());
    auto workspace=std::make_unique<Workspace>(directory,[]{});
    const auto state=Json::parse(workspace->diagnostics());
    require(state.at("open")&&state.at("capture")==source.capture&&state.at("selection_defined")&&state.at("start")==rate*2+1000,
        "Editor did not restore its native draft while offline.");
    require(state.at("playhead")==rate*2+1500,"Editor restart discarded the native cursor.");
    // A renderer/observer exception inside nested children must not poison
    // ImGui's next EndFrame or leave ordinary assertion checking disabled.
    ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.DisplaySize={1100,850};io.DeltaTime=1.f/60;
    io.IniFilename=nullptr;io.Fonts->Build();
    ImGui::NewFrame();Audio audio;bool injected=false;
    workspace->draw(audio,[&](const char* id,bool){
        if(std::string_view(id)=="sample-source-item-0"){injected=true;throw std::runtime_error("Injected editor draw failure");}
    });
    require(injected&&workspace->preparationStatus()=="Injected editor draw failure","Editor draw error was not reported.");
    require(io.ConfigErrorRecoveryEnableAssert,"Editor recovery disabled ordinary assertions.");
    ImGui::Render();ImGui::DestroyContext();
    WorkspaceReadinessTests::run(*workspace,audio);
    log<<"PASS editor audio readiness: restored, delayed, detached, stale and invalid descriptors across transport entry points\n";
    log<<"PASS editor draw exception restores nested ImGui state without disabling assertions\n";
    workspace->returnToPlayer();
    auto reopened=std::make_unique<Workspace>(directory,[]{});
    require(!Json::parse(reopened->diagnostics()).at("open").get<bool>(),"Restart ignored persisted player mode.");
    require(Json::parse(reopened->diagnostics()).at("playhead")==rate*2+1500,"Returning to player lost the saved cursor.");
    const auto previewDirectory=root/"restored-preview-range";fs::create_directories(previewDirectory);
    SampleLibrary previews;auto& preview=previews.open(source,total);
    preview.view={{"capture",capture},{"span_start",rate*5},{"span_end",rate*9},{"cursor",rate*6}};
    auto previewState=Json::parse(stateio::read(directory/"sample-workspaces.json"));
    previewState["workspaces"]=previews.json();previewState["active_source"]=source.json();previewState["open"]=true;
    stateio::write(previewDirectory/"sample-workspaces.json",previewState.dump());
    Workspace restoredPreview(previewDirectory,[]{});
    const auto restoredState=Json::parse(restoredPreview.diagnostics());
    require(!restoredState.at("selection_defined").get<bool>()&&restoredState.at("start")==rate*5&&restoredState.at("end")==rate*9,
        "Restart discarded the cue preview range without an active draft.");
    log<<"PASS native editor drafts and cursor restore offline without a second audio cache\n";
}

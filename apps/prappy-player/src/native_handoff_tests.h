#pragma once
#include "sample_workspace.h"
#include "evaluation_state.h"
#include <atomic>
inline void nativeHandoffTests(const fs::path& root,std::ostream& log){
    using namespace sampling;using namespace prepared;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto scope=root/"native-handoff",stable=scope/"stable",trial=scope/"trial",token=scope/"token";
    fs::create_directories(stable);fs::create_directories(trial);stateio::write(token,std::string(48,'t'));
    const std::string library="lib_"+std::string(32,'a'),capture="cap_"+std::string(32,'b'),sha(64,'c'),key(64,'d'),timeline="tl_"+std::string(32,'e');
    const Frame rate=44100,total=rate*8;
    const Json clock={{"id",timeline},{"capture_id",capture},{"sample_rate",rate},{"channels",2},{"frame_count",total},{"sample_format","float32"},{"stream_index",0}};
    const Json descriptor={{"source_key",key},{"timeline_id",timeline},{"frame_count",total},{"sample_rate",rate},{"channels",2},{"sample_format","float32"},{"access_mode","indexed"},
        {"capture",{{"id",capture},{"recording_id","rec_"+std::string(32,'f')},{"blob_sha256",sha},{"timeline_id",timeline},{"timeline",clock},
            {"recording",{{"title","Signal"}}},{"metadata",{{"effective",Json::object()}}}}}};
    auto reads=std::make_shared<std::atomic<int>>(0),lookups=std::make_shared<std::atomic<int>>(0);
    Client::Transport transport=[=](const Connection&,const Request& request,std::stop_token){
        Reply reply;reply.status=200;
        if(request.tag=="capabilities")reply.body={{"protocol",3},{"indexed_source",true},{"read_only",true}};
        else if(request.tag=="libraries")reply.body=Json::array({{{"id",library},{"name","Fixture"}}});
        else if(request.tag=="source-descriptor"||request.tag=="capture"||request.tag=="source-analysis")reply.body=descriptor;
        else if(request.tag=="mark-prepared"){++*lookups;reply.body=descriptor;}
        else if(request.tag=="source-pcm"){
            ++*reads;const Frame first=request.body.at("origin_frame"),count=request.body.at("frame_count");
            reply.preparedPcm=std::make_shared<Block>(NativeTimeline{rate,2,total,SampleType::Float32},first,count,std::vector<std::byte>(static_cast<size_t>(count)*8));
            reply.body={{"decoder_epoch","shared-native"}};
        }else if(request.tag=="source-waveform")reply.pcm=std::vector<float>(request.body.at("bin_count").get<size_t>()*4,.125f);
        else if(request.tag=="sourceRegions")reply.body={{"source",{{"library_id",library},{"capture_id",capture},{"timeline_id",timeline}}},{"items",Json::array()},{"offset",0},{"total",0}};
        else reply.body={{"items",Json::array()},{"total",0},{"active",true}};
        return reply;
    };
    stateio::write(trial/"evaluation-state.json",Json{{"mode","indexed-evaluation"},{"protocol",3},{"state_root",utf8(trial)},
        {"source_state_root",utf8(stable)},{"url","http://127.0.0.1:18816"},{"token_file",utf8(token)}}.dump());
    stateio::write(trial/"sampling-connection.json",Json{{"url","http://127.0.0.1:18816"},{"token_file",utf8(token)},{"library",library}}.dump());
    PcmBudget memory(64);Connection connection;connection.url="http://127.0.0.1:18816";connection.token=std::string(48,'t');connection.credentialFile=token;
    Audio audio;audio.openNative(true,memory);audio.nativeConnection=connection;audio.native=std::make_shared<NativePlayback>(connection,memory,[]{},transport);
    CatalogSource source;source.source={library,capture};source.asset=sha;source.state="Playable";source.native={rate,2,total,SampleType::Float32};source.duration=8;source.codec="aac";
    require(audio.playNative(trial/"signal.source",source,connection,true,2),"Native fixture load failed");
    const auto wait=[&](auto ready,auto tick){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);do{tick();if(ready())return;SDL_Delay(1);}while(std::chrono::steady_clock::now()<deadline);throw std::runtime_error("Native handoff timed out");};
    wait([&]{return audio.loaded&&audio.native->session()->covers(rate*2,rate*8);},[&]{audio.poll();});
    const auto shared=audio.native->session();const auto before=reads->load();const auto serial=audio.serial;
    Workspace workspace(trial,[]{},memory,transport);workspace.mark(audio);
    wait([&]{auto state=Json::parse(workspace.diagnostics());return state.at("shared_player_session")==true&&state.at("pcm_frames").get<Frame>()>0;},[&]{audio.poll();workspace.tick(audio);});
    require(audio.nativeEditing&&audio.paused&&audio.native->session()==shared,"Editor copied or replaced the active native session");
    require(reads->load()==before&&lookups->load()==0,"Cached editor handoff decoded or looked up the source again");
    workspace.toggleTransport(audio);
    wait([&]{return Json::parse(workspace.diagnostics()).at("audio_frames_submitted").get<Frame>()>0;},[&]{audio.poll();workspace.tick(audio);});
    require(audio.paused,"Editor audition resumed the ordinary player");
    workspace.returnToPlayer();audio.resumeNative();audio.poll();
    require(!audio.nativeEditing&&audio.paused&&audio.serial==serial&&audio.displayPosition()==2&&audio.native->session()==shared,"Return changed history identity, cue or PCM ownership");
    require(!fs::exists(trial/"sample-workspaces.json"),"Read-only handoff persisted an editing gesture");
    workspace.open=true;workspace.tick(audio);
    require(audio.nativeEditing&&audio.paused,"Restored editor left ordinary playback owning an active transport");
    workspace.returnToPlayer();audio.resumeNative();audio.poll();
    require(audio.native->session()==shared&&audio.displayPosition()==2,"Restored editor return replaced the native clock or PCM");
    Request fingerprint{"native-save-hash","LOCAL","",nullptr,0,{}};
    const auto reserved=memory.used();fingerprint.upload=audio.native->session()->clip(rate*2,rate*3);
    require(memory.used()==reserved,"Save snapshot copied or double-charged retained PCM");
    const auto checked=perform(connection,fingerprint);
    require(checked.error.empty()&&checked.body.at("sha256")=="19f0212a2c85ff556ebeb0e7ec8d5ac64299145a606f207ac650c4aab24bf73c"&&checked.body.at("bytes")==rate*8,"Off-thread save fingerprint changed native PCM bits");
    auto descriptorReady=std::make_shared<std::atomic<bool>>(false);
    Client::Transport delayed=[=](const Connection& c,const Request& request,std::stop_token stop){
        if(request.tag=="source-descriptor")while(!descriptorReady->load()&&!stop.stop_requested())SDL_Delay(1);
        return transport(c,request,stop);
    };
    audio.native=std::make_shared<NativePlayback>(connection,memory,[]{},delayed);
    require(audio.playNative(trial/"signal.source",source,connection,true,3),"Pending native fixture load failed");
    const auto inflight=audio.native;const auto pendingSerial=audio.serial;const auto previousLookups=lookups->load();
    Workspace pendingWorkspace(trial,[]{},memory,transport);pendingWorkspace.mark(audio);
    const bool adopted=audio.nativeEditing;descriptorReady->store(true);
    require(adopted,"Editor failed to adopt an in-flight native source");
    wait([&]{return Json::parse(pendingWorkspace.diagnostics()).at("pcm_frames").get<Frame>()>0;},[&]{audio.poll();pendingWorkspace.tick(audio);});
    require(audio.native==inflight&&lookups->load()==previousLookups,"In-flight handoff duplicated source lookup or transport");
    pendingWorkspace.returnToPlayer();audio.resumeNative();
    wait([&]{return audio.loaded;},[&]{audio.poll();});
    require(audio.native==inflight&&audio.serial==pendingSerial&&audio.displayPosition()==3,"In-flight return lost its original cue or history identity");
    log<<"PASS native handoff: shared immutable windows, in-flight adoption, no decode/lookup, independent audition and exact return cue\n";
}

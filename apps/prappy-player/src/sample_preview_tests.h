#pragma once
#include "sample_preview.h"
#include "sample_workspace.h"
#include <future>
#include <ostream>

inline void samplePreviewTests(const std::filesystem::path& root,std::ostream& log){
    using namespace sampling;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const auto rejects=[&](const auto& fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require(rejected,"Invalid restored preview accepted.");};
    const SourceRef source{"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b'),"tl_"+std::string(32,'c')};
    Preview preview{source,std::string(64,'d'),"verified-canonical-decoder",Rate*10,Rate*2,
        {{"frames_per_bin",Rate},{"peaks",{{-.1,.1,-.1,.1}}}},std::vector<float>(static_cast<size_t>(Rate)*2,.1f)};
    const auto encoded=preview.encode();const auto decoded=Preview::decode(encoded);
    require(decoded.matches(source,preview.blob,preview.total)&&decoded.pcm==preview.pcm,"Preview lost verified source identity or PCM.");
    require(!decoded.matches(source,std::string(64,'e'),preview.total)&&!decoded.matches(source,preview.blob,preview.total+1),"Preview was reused for changed canonical source.");
    auto broken=encoded;broken[broken.size()/2]^=1;rejects([&]{Preview::decode(broken);});
    auto invalid=preview;invalid.pcm[0]=std::numeric_limits<float>::infinity();rejects([&]{invalid.encode();});
    invalid=preview;invalid.start=invalid.total;rejects([&]{invalid.encode();});
    invalid=preview;invalid.overview["origin_frame"]=1;invalid.overview["frames_per_bin"]=std::numeric_limits<Frame>::max();rejects([&]{invalid.encode();});
    std::promise<void> entered,release,completed;
    auto gate=release.get_future().share();auto ready=completed.get_future();
    {
        PreviewStore store(root/"unused-preview.bin",[&]{completed.set_value();},[&](const auto&){entered.set_value();gate.wait();return encoded;});
        store.request(7);require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Preview read did not reach barrier.");
        require(!store.collect(),"Blocked preview read published data before validation.");
        release.set_value();require(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready,"Preview read did not finish after release.");
        const auto loaded=store.collect();require(loaded&&loaded->ticket==7&&loaded->value&&loaded->value->pcm==preview.pcm,"Async preview read lost its ticket.");
    }
    const auto directory=root/"restored-editor-preview";std::filesystem::create_directories(directory);
    const Json capture={{"id",source.capture},{"timeline_id",source.timeline},{"blob_sha256",preview.blob},
        {"timeline",{{"frame_count",preview.total}}},{"recording",{{"title","Saved sample"}}}};
    SampleLibrary samples;auto& ws=samples.open(source,preview.total);
    samples.select(source,{{Rate*2+1000,Rate*2+2000},{{"name","Saved draft"}}},"restored-draft");
    ws.view={{"capture",capture},{"viewport_start",Rate*2},{"viewport_end",Rate*3},{"cursor",Rate*2+1500}};
    stateio::write(directory/"sample-workspaces.json",Json{{"workspaces",samples.json()},{"active_source",source.json()},
        {"commands",Json::array()},{"open",true},{"browser_library",source.library},{"search",""},{"facet",""},{"facet_value",""},
        {"tab",0},{"offset",0},{"gain",.7}}.dump());
    stateio::write(directory/"sampling-connection.json",Json{{"url","http://127.0.0.1:1"},{"token_file",utf8(directory/"absent-token")},{"library",source.library}}.dump());
    stateio::write(directory/"sample-preview.bin",encoded);
    stateio::write(directory/"sample-preview.bin","interrupted/corrupt preview"); // valid backup must still restore offline
    std::mutex lock;std::condition_variable wake;uint64_t signals=0;
    auto workspace=std::make_unique<Workspace>(directory,[&]{std::lock_guard guard(lock);++signals;wake.notify_all();});
    Audio audio;
    auto state=Json::parse(workspace->diagnostics());
    require(state.at("open")&&state.at("capture")==source.capture&&state.at("selection_defined")&&state.at("start")==Rate*2+1000,
        "Editor did not restore its saved draft before connecting.");
    require(state.at("playhead")==Rate*2+1500,"Editor restart discarded the saved cursor.");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    uint64_t observed=0;
    while(state.at("pcm_frames")==0){
        std::unique_lock guard(lock);
        wake.wait_until(guard,deadline,[&]{return signals!=observed;});observed=signals;guard.unlock();
        workspace->tick(audio);state=Json::parse(workspace->diagnostics());
        if(std::chrono::steady_clock::now()>=deadline)break;
    }
    require(state.at("pcm_frames")==preview.pcm.size()/2&&state.at("pcm_start")==preview.start,
        "Cold editor restart waits for the backend instead of restoring its verified preview.");
    workspace->returnToPlayer();
    require(!Json::parse(stateio::read(directory/"sample-workspaces.json")).at("open").get<bool>(),
        "Returning to player checkpointed the editor as still open.");
    auto reopened=std::make_unique<Workspace>(directory,[]{});
    require(!Json::parse(reopened->diagnostics()).at("open").get<bool>(),"Restart ignored the persisted player mode.");
    require(Json::parse(reopened->diagnostics()).at("playhead")==Rate*2+1500,"Returning to player lost the saved editor cursor.");
    log<<"PASS saved editor preview: checked source/PCM, corrupt rejection, barrier-controlled async IO and offline draft/cursor/waveform restore\n";
}

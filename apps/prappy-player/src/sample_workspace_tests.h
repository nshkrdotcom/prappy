#pragma once
#include "sample_workspace.h"
#include <ostream>
inline void sampleWorkspaceTests(const std::filesystem::path& root,std::ostream& log){
    using namespace sampling;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const SourceRef source{"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b'),"tl_"+std::string(32,'c')};
    const Frame rate=44100,total=rate*10;
    const auto directory=root/"restored-native-editor";std::filesystem::create_directories(directory);
    const Json capture={{"id",source.capture},{"timeline_id",source.timeline},
        {"timeline",{{"frame_count",total},{"sample_rate",rate}}},{"recording",{{"title","Saved sample"}}}};
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
    workspace->returnToPlayer();
    auto reopened=std::make_unique<Workspace>(directory,[]{});
    require(!Json::parse(reopened->diagnostics()).at("open").get<bool>(),"Restart ignored persisted player mode.");
    require(Json::parse(reopened->diagnostics()).at("playhead")==rate*2+1500,"Returning to player lost the saved cursor.");
    log<<"PASS native editor drafts and cursor restore offline without a second audio cache\n";
}

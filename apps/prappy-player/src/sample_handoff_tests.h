#pragma once
#include "sample_workspace.h"
#include "sample_source_registration.h"
#include "state_io.h"
#include <ostream>

inline void pendingSampleHandoffTests(const fs::path& root,std::ostream& log) {
    using namespace sampling;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    {
        SourceHandoff handoff;
        const auto oversized=LibraryJson{{"path","song.wav"},{"action",0},{"cue",1e30},{"duration",1e30},{"selection",nullptr}};
        bool rejected=false;
        try{handoff.restore(oversized);}catch(const std::exception&){rejected=true;}
        require(rejected&&!handoff.current(),"Saved source time can overflow canonical frame conversion.");
    }
    {
        SourcePreparation preparation;
        preparation.tick(Connection{},[]{},"captured.wav",0,10,42);
        preparation.digest=std::string(64,'a');
        require(preparation.knownDigest("captured.wav",42)==preparation.digest&&
            preparation.knownDigest("captured.wav",43).empty()&&preparation.knownDigest("other.wav",42).empty(),
            "Listening digest was reused across different source paths or playback serials.");
    }
    require(sourceImportPath(L"\\\\wsl.localhost\\ubuntu26-dev\\home\\home\\Music\\a b.flac","http://127.0.0.1:8765")=="/home/home/Music/a b.flac",
        "WSL source registration path changed case or spaces.");
    require(sourceImportPath(L"\\\\?\\UNC\\wsl$\\ubuntu26-dev\\home\\home\\song.wav","http://localhost:18769")=="/home/home/song.wav",
        "Extended WSL source path was not mapped.");
    require(sourceImportPath(L"C:\\Music\\song.wav","http://[::1]:18769")=="/mnt/c/Music/song.wav",
        "Native drive source was not mapped to its WSL mount.");
    require(!sourceImportPath(L"\\\\wsl.localhost\\another-distro\\home\\song.wav","http://127.0.0.1:8765")&&
        !sourceImportPath(L"C:\\Music\\song.wav","https://127.0.0.1:8765@remote.example")&&
        !sourceImportPath(L"C:\\Music\\song.wav","https://remote.example")&&
        !sourceImportPath(L"\\\\nas\\Music\\song.wav","http://127.0.0.1:8765"),
        "Source registration accepted a remote host or unrelated filesystem namespace.");
    const auto directory=root/"pending-editor-handoff";
    fs::create_directories(directory);
    stateio::write(directory/"sampling-connection.json",Json{{"url","http://127.0.0.1:1"},
        {"token_file",utf8(directory/"absent-private-token")},{"library",""}}.dump());
    Audio source;source.current=root/"selected-original.wav";source.duration=10;source.position=2;
    {
        auto workspace=std::make_unique<Workspace>(directory,[]{});
        workspace->mark(source,SecondsSpan{1.25,2.5});
        const auto saved=Json::parse(stateio::read(directory/"sample-workspaces.json"));
        require(saved.contains("source_handoff")&&!saved.at("source_handoff").is_null(),
            "Pending source selection was not checkpointed before registration.");
        require(saved.at("source_handoff").at("selection")==Json{{"start",1.25},{"end",2.5}},
            "Pending source selection changed while checkpointing.");
    }
    {
        auto saved=Json::parse(stateio::read(directory/"sample-workspaces.json"));
        saved["source_handoff_digest"]=std::string(64,'a');
        saved["source_handoff_request"]="persisted-source-import-request";
        saved["source_handoff_library"]="persisted-import-library";
        stateio::write(directory/"sample-workspaces.json",saved.dump());
    }
    {
        auto reopened=std::make_unique<Workspace>(directory,[]{});
        const auto state=Json::parse(reopened->diagnostics());
        require(state.at("mark_pending")&&state.at("mark_frame")==Rate*2&&state.at("open"),
            "Restart discarded the pending source handoff or its captured cue.");
    }
    const auto restored=Json::parse(stateio::read(directory/"sample-workspaces.json"));
    require(restored.at("source_handoff_digest")==std::string(64,'a')&&
        restored.at("source_handoff_request")=="persisted-source-import-request"&&
        restored.at("source_handoff_library")=="persisted-import-library",
        "Restart discarded the frozen source identity or registration request.");
    log<<"PASS pending editor handoff: frozen source/cue/range persists before registration and restores after restart\n";
}

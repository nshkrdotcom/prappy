#pragma once
#include "sample_workspace.h"
#include "prepared_catalog.h"
#include "state_io.h"
#include <ostream>
inline void pendingSampleHandoffTests(const fs::path& root,std::ostream& log) {
    using namespace sampling;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto directory=root/"pending-editor-handoff";
    fs::create_directories(directory);
    stateio::write(directory/"sampling-connection.json",Json{{"url","http://127.0.0.1:1"},
        {"token_file",utf8(directory/"absent-private-token")},{"library",""}}.dump());
    auto selected=std::make_shared<prepared::CatalogSource>();
    selected->source={"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b')};
    selected->asset=std::string(64,'c');selected->state="Ready";
    selected->native={44100,1,441000,prepared::SampleType::Float64};
    Audio source;source.current=root/"virtual-source";source.duration=10;source.position=2;source.preparedSource=selected;
    {
        auto workspace=std::make_unique<Workspace>(directory,[]{});
        workspace->mark(source,SecondsSpan{1.25,2.5});
        const auto saved=Json::parse(stateio::read(directory/"sample-workspaces.json"));
        require(saved.at("source_handoff").at("selection")==Json{{"start",1.25},{"end",2.5}},"Pending selection changed while checkpointing.");
        require(saved.at("source_handoff_source")==Json{{"library",selected->source.library},{"capture",selected->source.capture},
            {"asset",selected->asset},{"sample_rate",44100}},"Editor did not freeze the loaded prepared source identity.");
        require(!saved.contains("source_handoff_digest")&&!saved.contains("source_handoff_request"),"Editor still checkpoints source hashing or registration work.");
    }
    {
        auto reopened=std::make_unique<Workspace>(directory,[]{});
        const auto state=Json::parse(reopened->diagnostics());
        require(state.at("mark_pending")&&state.at("mark_frame")==88200&&state.at("open"),"Restart discarded the pending source handoff or native cue.");
    }
    log<<"PASS pending editor handoff: immutable prepared source and native cue/range survive restart\n";
}

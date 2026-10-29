#pragma once
#include "sample_handoff.h"
#include "state_io.h"
#include <ostream>

inline void sampleLibraryTests(const std::filesystem::path& directory,std::ostream& log){
    using namespace sampling;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const auto rejects=[&](const auto& fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require(rejected,"Invalid source/sample operation was accepted.");};
    const SourceRef a{"lib_"+std::string(32,'a'),"cap_"+std::string(32,'b'),"tl_"+std::string(32,'c')};
    const SourceRef b{a.library,"cap_"+std::string(32,'d'),"tl_"+std::string(32,'e')};
    const SourceRef alternateTimeline{a.library,a.capture,b.timeline};
    SampleLibrary library;library.open(a,Rate*10);library.open(b,Rate*10);library.open(alternateTimeline,Rate*10);
    const DraftValue first{{1000,2000},{{"name","First"},{"kind","unknown"}}};
    const auto one=library.select(a,first,"draft-1");
    require(library.select(a,first,"should-not-exist")==one&&library.at(a).drafts.size()==1,"Repeated range created another draft.");
    library.select(a,{{3000,4000},{{"name","Second"}}},"draft-2");
    library.select(b,first,"draft-1");library.select(alternateTimeline,first,"draft-1");
    require(library.all().size()==3&&library.at(a).drafts.size()==2,"Source/timeline workspaces collided.");
    library.at(a).view={{"viewport_start",900},{"viewport_end",2200},{"amplitude_mode",2}};
    const auto command=[&](std::string request,Span span,std::optional<RegionRef> ref={}){
        LibraryJson payload={{"capture_id",a.capture},{"name",ref?"Edited while saving":"First"},{"kind","unknown"},{"span",{{"start_frame",span.start},{"end_frame",span.end}}}};
        if(ref){payload["id"]=ref->id;payload["expected"]=ref->revision;}
        return LibraryJson{{"request_id",request},{"operation","region.save"},{"payload",payload}};
    };
    const auto savedRegion=[&](int revision,Span span){return LibraryJson{{"id","reg_"+std::string(32,'f')},{"revision",revision},{"capture_id",a.capture},{"timeline_id",a.timeline},{"span",{{"start_frame",span.start},{"end_frame",span.end}}}};};
    const auto frozen=library.beginSave(a,one,command("request-1",first.span));
    library.edit(a,one,{{1001,2001},{{"name","Edited while saving"}}});
    require(library.beginSave(a,one,command("request-new",{1001,2001}))==frozen,"Retry replaced an in-flight payload.");
    const auto file=directory/"sample-workspaces-test.json";
    stateio::write(file,library.json().dump());
    auto reopened=SampleLibrary::parse(LibraryJson::parse(stateio::read(file)));
    require(reopened.json()==library.json(),"Multiple drafts/view/pending save did not survive atomic persistence.");
    require(!reopened.acknowledge(a,one,"stale-request",savedRegion(1,first.span)),"Stale response modified a draft.");
    require(reopened.acknowledge(a,one,"request-1",savedRegion(1,first.span)),"Save acknowledgment was lost after restart/source switch.");
    require(reopened.draft(a,one).dirty()&&reopened.draft(a,one).value.span==Span{1001,2001},"Save response destroyed newer edits.");
    require(reopened.draft(b,one).value==first&&!reopened.draft(b,one).region,"Late response changed another source.");
    require(!reopened.acknowledge(a,one,"request-1",savedRegion(1,first.span)),"Duplicate acknowledgment was applied twice.");
    const auto ref=reopened.draft(a,one).region;
    reopened.beginSave(a,one,command("request-2",{1001,2001},ref));
    reopened.conflict(a,one,"request-2");
    auto conflicted=SampleLibrary::parse(reopened.json());
    require(conflicted.draft(a,one).saving->conflict&&conflicted.draft(a,one).dirty(),"Conflict lost the draft or frozen retry payload.");
    rejects([&]{conflicted.discard(a,one);});
    require(reopened.acknowledge(a,one,"request-2",savedRegion(2,{1001,2001}))&&!reopened.draft(a,one).dirty(),"Second save did not advance the same region.");
    require(reopened.selectSaved(a,savedRegion(2,{1001,2001}),reopened.draft(a,one).value,"unused")==one,"Saved region was duplicated.");
    rejects([&]{reopened.selectSaved(b,savedRegion(2,{1001,2001}),first,"bad");});
    rejects([&]{reopened.open(a,1);});
    rejects([&]{reopened.edit(a,one,{{0,Rate*11},{}});});
    auto corrupt=reopened.json();corrupt["sources"].push_back(corrupt["sources"][0]);
    rejects([&]{SampleLibrary::parse(corrupt);});
    reopened.discard(a,"draft-2");require(reopened.at(a).drafts.size()==1,"Discard removed the wrong draft.");
    SourceHandoff handoff;
    const auto stale=handoff.begin("alias-one",OpenSource::Cue,1,10);
    const auto requested=handoff.begin("alias-two",OpenSource::Selection,2,10,SecondsSpan{1.25,2.5});
    require(!handoff.resolve(stale.ticket,b,Rate*10),"Superseded handoff was accepted.");
    const auto resolved=handoff.resolve(requested.ticket,a,Rate*10);
    require(resolved&&resolved->source==a&&resolved->selection==Span{60000,120000}&&!handoff.current(),"Selection/cue mapping lost frozen intent.");
    const auto cancelled=handoff.begin("cancel",OpenSource::Samples,0,10);handoff.cancel();
    require(!handoff.resolve(cancelled.ticket,a,Rate*10),"Cancelled handoff was accepted.");
    const auto mismatch=handoff.begin("mismatch",OpenSource::Selection,1,10,SecondsSpan{1,2});
    rejects([&]{handoff.resolve(mismatch.ticket,a,Rate*9);});
    require(handoff.current()->ticket==mismatch.ticket,"Timeline mismatch discarded recoverable intent.");
    rejects([&]{handoff.begin("bad",OpenSource::Selection,0,10,SecondsSpan{2,2});});
    log<<"PASS canonical multi-source drafts, atomic reopen, idempotent pending saves, late acknowledgments, conflicts and source/range intent fencing\n";
}

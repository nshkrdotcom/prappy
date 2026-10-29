#pragma once
#include "sample_editor.h"
#include <json.hpp>
#include <map>
#include <string>
#include <compare>
#include <stdexcept>
#include <set>

namespace sampling {
using LibraryJson = nlohmann::json;

// Persistent identity never contains a path, title or browser position.
struct SourceRef {
    std::string library,capture,timeline;
    auto operator<=>(const SourceRef&)const=default;
    static bool id(const std::string& value,const std::string& prefix){
        return value.starts_with(prefix)&&value.size()==prefix.size()+32&&
            value.find_first_not_of("0123456789abcdef",prefix.size())==std::string::npos;
    }
    void validate()const{
        if(!id(library,"lib_")||!id(capture,"cap_")||!id(timeline,"tl_"))throw std::runtime_error("Invalid canonical source reference.");
    }
    LibraryJson json()const{return {{"library_id",library},{"capture_id",capture},{"timeline_id",timeline}};}
    static SourceRef parse(const LibraryJson& j){SourceRef s{j.at("library_id"),j.at("capture_id"),j.at("timeline_id")};s.validate();return s;}
};
struct RegionRef {
    std::string id;int revision=1;
    bool operator==(const RegionRef&)const=default;
};
struct DraftValue {
    Span span;
    // Editor-owned name, kind, metadata edits etc. No duplicate span/source authority.
    LibraryJson attributes=LibraryJson::object();
    bool operator==(const DraftValue&)const=default;
    LibraryJson json()const{return {{"start",span.start},{"end",span.end},{"attributes",attributes}};}
    static DraftValue parse(const LibraryJson& j){return {{j.at("start"),j.at("end")},j.at("attributes")};}
};
struct DraftSave {
    std::string request;
    DraftValue submitted;
    LibraryJson command;
    bool conflict=false;
};
struct SampleDraft {
    std::string id;
    DraftValue value;
    std::optional<RegionRef> region;
    std::optional<DraftValue> baseline;
    std::optional<DraftSave> saving;
    bool dirty()const{return !baseline||value!=*baseline;}
};
struct SourceWorkspace {
    Frame total=0;
    std::string active;
    LibraryJson view=LibraryJson::object();
    std::map<std::string,SampleDraft> drafts;
};

// UI-thread model. Source switches do not discard drafts, pending commands or view state.
// The adapter persists json() atomically before sending commands and after acknowledgments.
class SampleLibrary {
    std::map<SourceRef,SourceWorkspace> sources;
    static void checkValue(const DraftValue& value,Frame total){
        if(total<=0||value.span.start<0||value.span.start>=value.span.end||value.span.end>total||!value.attributes.is_object())
            throw std::runtime_error("Invalid sample draft bounds or attributes.");
    }
    static void checkName(const std::string& name){if(name.empty()||name.size()>128)throw std::runtime_error("Invalid local draft identity.");}
    static RegionRef regionRef(const LibraryJson& j){
        RegionRef r{j.at("id"),j.at("revision")};
        if(!SourceRef::id(r.id,"reg_")||r.revision<1)throw std::runtime_error("Invalid saved region reference.");
        return r;
    }
    static void checkCommand(const SourceRef& source,const SampleDraft& draft,const DraftSave& save){
        const auto& c=save.command;const auto& p=c.at("payload");
        if(c.at("request_id")!=save.request||c.at("operation")!="region.save"||p.at("capture_id")!=source.capture||
           p.at("span").at("start_frame")!=save.submitted.span.start||p.at("span").at("end_frame")!=save.submitted.span.end)
            throw std::runtime_error("Save command does not describe this draft.");
        for(const auto* field:{"name","kind"})if(save.submitted.attributes.contains(field)&&p.at(field)!=save.submitted.attributes.at(field))
            throw std::runtime_error("Save command metadata differs from the submitted draft.");
        if(draft.region){
            if(p.at("id")!=draft.region->id||p.at("expected")!=draft.region->revision)throw std::runtime_error("Wrong expected region revision.");
        }else if(p.contains("id")&&!p.at("id").is_null())throw std::runtime_error("New draft cannot overwrite a saved sample.");
    }
public:
    const auto& all()const{return sources;}
    SourceWorkspace& open(const SourceRef& source,Frame total){
        source.validate();if(total<=0)throw std::runtime_error("Empty canonical timeline.");
        const auto it=sources.find(source);
        if(it!=sources.end()){
            if(it->second.total!=total)throw std::runtime_error("Canonical timeline length changed.");
            return it->second;
        }
        SourceWorkspace ws;ws.total=total;
        return sources.emplace(source,std::move(ws)).first->second;
    }
    SourceWorkspace& at(const SourceRef& source){return sources.at(source);}
    const SourceWorkspace& at(const SourceRef& source)const{return sources.at(source);}
    SampleDraft& draft(const SourceRef& source,const std::string& id){return sources.at(source).drafts.at(id);}
    // Same range focuses existing work, including a pending save. New variants must be explicit.
    std::string select(const SourceRef& source,DraftValue value,const std::string& newId,bool variant=false){
        auto& ws=sources.at(source);checkValue(value,ws.total);checkName(newId);
        if(!variant)for(const auto& [id,d]:ws.drafts)if(d.value.span==value.span){ws.active=id;return id;}
        if(ws.drafts.contains(newId))throw std::runtime_error("Draft ID already exists.");
        ws.drafts.emplace(newId,SampleDraft{newId,std::move(value),{},{},{}});ws.active=newId;return newId;
    }
    void edit(const SourceRef& source,const std::string& id,DraftValue value){checkValue(value,at(source).total);draft(source,id).value=std::move(value);}
    // Existing drafts keep their base revision and unsaved edits when server summaries refresh.
    std::string selectSaved(const SourceRef& source,const LibraryJson& region,DraftValue value,const std::string& newId){
        if(region.at("capture_id")!=source.capture||region.at("timeline_id")!=source.timeline||
           region.at("span").at("start_frame")!=value.span.start||region.at("span").at("end_frame")!=value.span.end)
            throw std::runtime_error("Region belongs to another source or span.");
        const auto ref=regionRef(region);auto& ws=at(source);checkValue(value,ws.total);checkName(newId);
        for(const auto& [id,d]:ws.drafts)if(d.region&&d.region->id==ref.id){ws.active=id;return id;}
        if(ws.drafts.contains(newId))throw std::runtime_error("Draft ID already exists.");
        ws.drafts.emplace(newId,SampleDraft{newId,value,ref,value,{}});ws.active=newId;return newId;
    }
    LibraryJson beginSave(const SourceRef& source,const std::string& id,LibraryJson command){
        auto& d=draft(source,id);
        if(d.saving)return d.saving->command; // Retry always sends the original immutable payload.
        DraftSave save{command.at("request_id"),d.value,std::move(command),false};checkName(save.request);
        for(const auto& [s,ws]:sources)for(const auto& [key,other]:ws.drafts)
            if(other.saving&&other.saving->request==save.request)throw std::runtime_error("Request ID already belongs to a draft.");
        checkCommand(source,d,save);d.saving=std::move(save);return d.saving->command;
    }
    bool acknowledge(const SourceRef& source,const std::string& id,const std::string& request,const LibraryJson& region){
        auto& d=draft(source,id);if(!d.saving||d.saving->request!=request)return false;
        const auto ref=regionRef(region);const auto& submitted=d.saving->submitted;
        if(region.at("capture_id")!=source.capture||region.at("timeline_id")!=source.timeline||
           region.at("span").at("start_frame")!=submitted.span.start||region.at("span").at("end_frame")!=submitted.span.end||
           (d.region?(ref.id!=d.region->id||ref.revision!=d.region->revision+1):ref.revision!=1))
            throw std::runtime_error("Save response does not match the submitted sample.");
        for(const auto& [key,other]:at(source).drafts)if(key!=id&&other.region&&other.region->id==ref.id)
            throw std::runtime_error("Region already belongs to another draft.");
        d.region=ref;d.baseline=submitted;d.saving.reset();return true; // Newer edits remain dirty.
    }
    void conflict(const SourceRef& source,const std::string& id,const std::string& request){
        auto& d=draft(source,id);if(d.saving&&d.saving->request==request)d.saving->conflict=true;
    }
    void reloadSaved(const SourceRef& source,const std::string& id,const LibraryJson& region,DraftValue value){
        auto& d=draft(source,id);const auto ref=regionRef(region);checkValue(value,at(source).total);
        if(!d.region||d.region->id!=ref.id||region.at("capture_id")!=source.capture||region.at("timeline_id")!=source.timeline||
           region.at("span").at("start_frame")!=value.span.start||region.at("span").at("end_frame")!=value.span.end)
            throw std::runtime_error("Reload does not describe this saved sample.");
        if(d.saving&&!d.saving->conflict)throw std::runtime_error("Wait for the pending save before reloading.");
        d.value=value;d.baseline=value;d.region=ref;d.saving.reset();
    }
    void discard(const SourceRef& source,const std::string& id){
        auto& ws=at(source);const auto& d=ws.drafts.at(id);
        if(d.region||d.saving)throw std::runtime_error("Saved or pending samples cannot be discarded as drafts.");
        ws.drafts.erase(id);if(ws.active==id)ws.active.clear();
    }
    LibraryJson json()const{
        auto rows=LibraryJson::array();
        for(const auto& [source,ws]:sources){
            auto drafts=LibraryJson::array();
            for(const auto& [id,d]:ws.drafts){
                LibraryJson row={{"id",id},{"value",d.value.json()},{"region",nullptr},{"baseline",nullptr},{"saving",nullptr}};
                if(d.region)row["region"]={{"id",d.region->id},{"revision",d.region->revision}};
                if(d.baseline)row["baseline"]=d.baseline->json();
                if(d.saving)row["saving"]={{"request",d.saving->request},{"submitted",d.saving->submitted.json()},{"command",d.saving->command},{"conflict",d.saving->conflict}};
                drafts.push_back(std::move(row));
            }
            rows.push_back({{"source",source.json()},{"total",ws.total},{"active",ws.active},{"view",ws.view},{"drafts",drafts}});
        }
        return {{"sources",rows}};
    }
    static SampleLibrary parse(const LibraryJson& data){
        SampleLibrary result;std::set<std::string> requests;
        if(!data.at("sources").is_array())throw std::runtime_error("Expected source workspace list.");
        for(const auto& row:data.at("sources")){
            const auto source=SourceRef::parse(row.at("source"));
            if(result.sources.contains(source))throw std::runtime_error("Duplicate canonical source.");
            auto& ws=result.open(source,row.at("total"));ws.active=row.at("active");ws.view=row.at("view");
            if(!ws.view.is_object()||!row.at("drafts").is_array())throw std::runtime_error("Invalid source view or draft list.");
            for(const auto& entry:row.at("drafts")){
                SampleDraft d;d.id=entry.at("id");checkName(d.id);d.value=DraftValue::parse(entry.at("value"));checkValue(d.value,ws.total);
                if(!entry.at("region").is_null())d.region=regionRef(entry.at("region"));
                if(!entry.at("baseline").is_null()){d.baseline=DraftValue::parse(entry.at("baseline"));checkValue(*d.baseline,ws.total);}
                if(d.region.has_value()!=d.baseline.has_value())throw std::runtime_error("Saved sample lacks its baseline.");
                if(!entry.at("saving").is_null()){
                    const auto& save=entry.at("saving");d.saving=DraftSave{save.at("request"),DraftValue::parse(save.at("submitted")),save.at("command"),save.at("conflict")};
                    checkName(d.saving->request);checkValue(d.saving->submitted,ws.total);checkCommand(source,d,*d.saving);
                    if(!requests.insert(d.saving->request).second)throw std::runtime_error("Duplicate pending request.");
                }
                for(const auto& [id,other]:ws.drafts)if(d.region&&other.region&&d.region->id==other.region->id)throw std::runtime_error("Duplicate region draft.");
                if(!ws.drafts.emplace(d.id,std::move(d)).second)throw std::runtime_error("Duplicate local draft.");
            }
            if(!ws.active.empty()&&!ws.drafts.contains(ws.active))throw std::runtime_error("Missing active draft.");
        }
        return result;
    }
};
}

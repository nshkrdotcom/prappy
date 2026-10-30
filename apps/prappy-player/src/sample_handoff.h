#pragma once
#include "sample_library.h"
#include <limits>

namespace sampling {
enum class OpenSource { Cue, Selection, Samples };
struct SecondsSpan {double start=0,end=0;};
struct SourceIntent {
    uint64_t ticket=0;
    std::string path;
    OpenSource action=OpenSource::Cue;
    double cue=0,duration=0;
    std::optional<SecondsSpan> selection;
};
struct ResolvedIntent {
    SourceIntent intent;
    SourceRef source;
    Frame total=0,cue=0;
    std::optional<Span> selection;
};
// A frozen UI gesture is resolved against its source, never the current player song.
// Supersession/cancel reject late completions without touching any workspace.
class SourceHandoff {
    uint64_t sequence=0;
    std::optional<SourceIntent> pending;
public:
    const auto& current()const{return pending;}
    LibraryJson snapshot()const{
        if(!pending)return nullptr;
        LibraryJson value={{"path",pending->path},{"action",static_cast<int>(pending->action)},
            {"cue",pending->cue},{"duration",pending->duration},{"selection",nullptr}};
        if(pending->selection)value["selection"]={{"start",pending->selection->start},{"end",pending->selection->end}};
        return value;
    }
    void restore(const LibraryJson& value){
        pending.reset();if(value.is_null())return;
        const auto action=value.at("action").get<int>();
        if(action<0||action>static_cast<int>(OpenSource::Samples))throw std::runtime_error("Invalid saved source action.");
        std::optional<SecondsSpan> selection;
        if(!value.at("selection").is_null())selection=SecondsSpan{value.at("selection").at("start"),value.at("selection").at("end")};
        begin(value.at("path"),static_cast<OpenSource>(action),value.at("cue"),value.at("duration"),selection);
    }
    SourceIntent begin(std::string path,OpenSource action,double cue,double duration,std::optional<SecondsSpan> selection={}){
        if(path.empty()||!std::isfinite(duration)||duration<=0||
           duration*Rate>=static_cast<double>(std::numeric_limits<Frame>::max())||
           !std::isfinite(cue)||cue<0||cue>duration||
           ((action==OpenSource::Selection)!=selection.has_value()))throw std::runtime_error("Invalid source handoff.");
        if(selection&&(!std::isfinite(selection->start)||!std::isfinite(selection->end)||selection->start<0||
           selection->start>=selection->end||selection->end>duration))throw std::runtime_error("Invalid player selection.");
        pending=SourceIntent{++sequence,std::move(path),action,cue,duration,selection};return *pending;
    }
    void cancel(){pending.reset();}
    std::optional<ResolvedIntent> resolve(uint64_t ticket,const SourceRef& source,Frame total){
        if(!pending||pending->ticket!=ticket)return {};
        source.validate();
        // 50 ms tolerates duration reporting granularity, not a different edit/timeline.
        if(total<=0||std::abs(pending->duration-static_cast<double>(total)/Rate)>.05)
            throw std::runtime_error("Player duration differs from the canonical timeline. Select again in the editor.");
        const auto frame=[&](double seconds){
            const double scaled=seconds*Rate;
            if(!std::isfinite(scaled)||scaled>=static_cast<double>(std::numeric_limits<Frame>::max()))throw std::runtime_error("Time exceeds frame range.");
            const auto value=static_cast<Frame>(std::llround(scaled));
            // Only sub-frame duration-rounding error at the source end may be clamped.
            if(value>total&&scaled-static_cast<double>(total)>.5)throw std::runtime_error("Position is outside the canonical timeline.");
            return std::min(total,value);
        };
        ResolvedIntent result{*pending,source,total,frame(pending->cue),{}};
        if(pending->selection){
            result.selection=Span{frame(pending->selection->start),frame(pending->selection->end)};
            if(result.selection->start>=result.selection->end)throw std::runtime_error("Selection is shorter than a canonical sample.");
        }
        pending.reset();return result;
    }
};
}

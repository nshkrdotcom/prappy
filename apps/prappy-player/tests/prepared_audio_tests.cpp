#include "prepared_audio.h"
#include "prepared_catalog.h"
#include "prepared_requests.h"
#include "sample_handoff.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool condition) { if (!condition) throw std::runtime_error("prepared audio contract failed"); }
template<class Action> void rejects(Action action) {
    bool failed=false; try { action(); } catch(const std::exception&) { failed=true; }
    require(failed);
}
int main() {
    using namespace prepared;
    const std::string library="lib_"+std::string(32,'a'),capture="cap_"+std::string(32,'b');
    SourceRef source{library,capture};source.validate();
    rejects([&]{SourceRef{"../../elsewhere",capture}.validate();});
    require(source.endpoint()=="/v1/libraries/"+library+"/captures/"+capture+"/prepared");
    require(scaleFloor(44101,44100,48000)==48001);
    require(scaleCeil(44101,44100,48000)==48002);
    require(scaleFloor(48000,48000,44100)==44100);
    require(scaleFloor(385142701,48000,48000)==385142701);
    require(scaleFloor(5000000000LL,96000,48000)==2500000000LL);
    rejects([]{scaleFloor(-1,44100,48000);});
    rejects([]{scaleFloor(1,0,48000);});
    NativeTimeline timeline{96000,6,5000000000LL,SampleType::Float64};timeline.validate();
    require(timeline.seconds(96000)==1);
    require(timeline.fromSeconds(1.5)==144000);
    rejects([&]{timeline.fromSeconds(-1);});
    const double precise=.125+1.0/1099511627776.0;
    std::vector<std::byte> bytes(sizeof(precise));std::memcpy(bytes.data(),&precise,sizeof(precise));
    Block block({44100,1,1,SampleType::Float64},0,1,bytes);
    require(block.sample(0,0)==precise);
    require(block.bytes()==bytes);
    const int32_t integer=268435457;
    bytes.resize(sizeof(integer));std::memcpy(bytes.data(),&integer,sizeof(integer));
    Block integerBlock({48000,1,1,SampleType::Int32},0,1,bytes);
    require(integerBlock.sample(0,0)==static_cast<double>(integer)/2147483648.0);
    rejects([&]{Block malformed({48000,2,1,SampleType::Float32},0,1,bytes);});
    rejects([&]{block.sample(1,0);});
    nlohmann::json row={{"library_id",library},{"capture_id",capture},{"timeline_id","tl_"+std::string(32,'d')},{"asset_key",std::string(64,'c')},
        {"state","Ready"},{"title","Test song"},{"artist",nullptr},{"codec","opus"},
        {"library_name","Music"},{"duration_seconds",2.0},{"original_bytes",1200},{"original_path","/original/music"},
        {"analysis",{{"native_frames",88200},{"native_rate",44100},{"native_channels",1},
            {"native_sample_format","float64"},{"listening_frames",96000},{"frames",96000},
            {"sample_rate",48000},{"channels",2},{"loudness_lufs",-23.0},{"sample_peak",.5},
            {"source_tags",{{"R128_TRACK_GAIN","-512"}}},{"source_codec","opus"}}}};
    const auto catalog=parseCatalog({{"items",nlohmann::json::array({row})}});
    require(catalog.size()==1&&catalog[0].source==source&&catalog[0].native.rate==44100);
    require(catalog[0].native.type==SampleType::Float64&&catalog[0].loudness==-23.0);
    require(catalog[0].tags[0].first=="R128_TRACK_GAIN");
    rejects([&]{parseCatalog({{"items",nlohmann::json::array({row,row})}});});
    row["analysis"]["native_frames"]=1.5;
    rejects([&]{parseCatalog({{"items",nlohmann::json::array({row})}});});
    sampling::SourceHandoff handoff;
    const auto intent=handoff.begin("stable-recording",sampling::OpenSource::Selection,1,2,sampling::SecondsSpan{.5,1.5});
    const auto resolved=handoff.resolve(intent.ticket,{library,capture,"tl_"+std::string(32,'d')},88200,44100);
    require(resolved&&resolved->selection&&resolved->selection->start==22050&&resolved->selection->end==66150);
    nlohmann::json manifest={{"key",std::string(64,'a')},{"sample_rate",192000},{"channels",32},
        {"frames",1920000},{"sample_format","float64"},{"capture",{{"timeline_id","tl_"+std::string(32,'d')}}}};
    require(pcmContext(manifest,13,100).at("channels")==32);
    rejects([&]{pcmContext(manifest,0,1920000);});
    rejects([&]{pcmContext(manifest,1920000,1);});
    std::cout<<"PASS prepared source identity, native timeline, precision and bounded payloads\n";
}

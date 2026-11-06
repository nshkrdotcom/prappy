#include "prepared_audio.h"
#include "native_session.h"
#include "analysis_dsp.h"
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
    require(source.endpoint()=="/v1/libraries/"+library+"/captures/"+capture+"/audio");
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
    nlohmann::json row={{"library_id",library},{"capture_id",capture},{"original_sha256",std::string(64,'c')},
        {"state","Playable"},{"analysis_state","Queued"},{"title","Test song"},{"artist",nullptr},
        {"library_name","Music"},{"original_bytes",1200},{"original_path","/original/music"},
        {"media",{{"codec","aac"},{"sample_rate",44100},{"channels",1},{"duration_seconds",2.0}}}};
    const auto catalog=parseCatalog({{"protocol",3},{"items",nlohmann::json::array({row})}});
    require(catalog.size()==1&&catalog[0].source==source&&catalog[0].native.rate==44100);
    require(catalog[0].native.frames==0&&catalog[0].ready()&&!catalog[0].loudness);
    require(catalog[0].timeline.empty());
    rejects([&]{parseCatalog({{"protocol",3},{"items",nlohmann::json::array({row,row})}});});
    rejects([&]{parseCatalog({{"protocol",2},{"items",nlohmann::json::array({row})}});});
    row["media"]["sample_rate"]=1.5;
    rejects([&]{parseCatalog({{"protocol",3},{"items",nlohmann::json::array({row})}});});
    sampling::SourceHandoff handoff;
    const auto intent=handoff.begin("stable-recording",sampling::OpenSource::Selection,1,2,sampling::SecondsSpan{.5,1.5});
    const auto resolved=handoff.resolve(intent.ticket,{library,capture,"tl_"+std::string(32,'d')},88200,44100);
    require(resolved&&resolved->selection&&resolved->selection->start==22050&&resolved->selection->end==66150);
    nlohmann::json manifest={{"source_key",std::string(64,'a')},{"sample_rate",192000},{"channels",32},
        {"frame_count",1920000},{"sample_format","float64"},{"timeline_id","tl_"+std::string(32,'d')}};
    require(pcmContext(manifest,13,100).at("channels")==32);
    rejects([&]{pcmContext(manifest,0,1920000);});
    rejects([&]{pcmContext(manifest,1920000,1);});
    analysis::Result nativeWaveform;nativeWaveform.frames=44100;nativeWaveform.sampleRate=44100;
    require(nativeWaveform.duration()==1.);
    sampling::PcmBudget memory(64);
    auto incoming=memory.reserve(40ULL*1024*1024);
    rejects([&]{memory.reserve(25ULL*1024*1024);});
    auto undo=incoming;incoming.reset();require(memory.used()==40ULL*1024*1024);
    undo.reset();require(memory.used()==0);
    require(sampling::PcmBudget{}.limit()==256ULL*1024*1024);
    rejects([]{sampling::PcmBudget invalid(32);});
    const NativeTimeline high{192000,1,5000000000LL,SampleType::Float64};
    auto native=std::make_shared<const Block>(high,4999999999LL,1,
        std::as_bytes(std::span(&precise,size_t{1})));
    sampling::NativeSession session(high,"source-key","timeline",7,memory);
    auto stored=session.adopt(native,memory.reserve(8),"source-key","timeline",7,"decoder-a");
    require(session.covers(4999999999LL,5000000000LL)&&!session.covers(4999999998LL,5000000000LL));
    rejects([&]{session.adopt(native,memory.reserve(8),"source-key","timeline",7,"decoder-b");});
    require(stored&&memory.used()==8);
    require(session.find(4999999999LL)->pcm->sample(0,0)==precise);
    require(!session.adopt(native,memory.reserve(8),"source-key","timeline",6));
    require(memory.used()==8);
    rejects([&]{session.adopt(native,memory.reserve(8),"other-key","timeline",7);});
    session.retain(0,1);require(!session.find(4999999999LL)&&memory.used()==8);
    stored.reset();require(memory.used()==0);    std::cout<<"PASS prepared source identity, native timeline, precision and bounded payloads\n";
}

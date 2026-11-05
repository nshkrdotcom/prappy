#include "prepared_audio.h"
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
    std::cout<<"PASS prepared source identity, native timeline, precision and bounded payloads\n";
}

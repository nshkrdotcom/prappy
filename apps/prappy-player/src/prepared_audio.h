#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prepared {
using Frame=int64_t;
enum class SampleType { Float32, Int32, Float64 };
inline SampleType sampleType(std::string_view name) {
    if(name=="float32")return SampleType::Float32;
    if(name=="int32")return SampleType::Int32;
    if(name=="float64")return SampleType::Float64;
    throw std::runtime_error("Unknown prepared PCM sample type.");
}
inline size_t sampleBytes(SampleType type) {
    switch(type){case SampleType::Float32:case SampleType::Int32:return 4;case SampleType::Float64:return 8;}
    throw std::runtime_error("Invalid prepared PCM sample type.");
}
struct SourceRef {
    std::string library,capture;
    static bool valid(std::string_view value,std::string_view prefix) {
        return value.starts_with(prefix)&&value.size()==prefix.size()+32&&
            value.find_first_not_of("0123456789abcdef",prefix.size())==std::string_view::npos;
    }
    void validate()const {
        if(!valid(library,"lib_")||!valid(capture,"cap_"))throw std::runtime_error("Invalid prepared source identity.");
    }
    std::string endpoint()const {validate();return "/v1/libraries/"+library+"/captures/"+capture+"/audio";}
    bool operator==(const SourceRef&)const=default;
};
// Avoid count + chunk - 1: a valid int64 source can overflow that sum.
inline Frame ceilDivPositive(Frame count,Frame chunk){
    if(count<0||chunk<=0)throw std::runtime_error("Invalid native waveform span.");
    return count/chunk+(count%chunk!=0);
}
inline Frame scaleFloor(Frame frame,int64_t fromRate,int64_t toRate) {
    if(frame<0||fromRate<=0||fromRate>768000||toRate<=0||toRate>768000)
        throw std::runtime_error("Invalid source frame conversion.");
    const auto whole=frame/fromRate,remainder=frame%fromRate;
    const auto tail=remainder*toRate/fromRate;
    if(whole>(std::numeric_limits<Frame>::max()-tail)/toRate)
        throw std::runtime_error("Source frame conversion overflow.");
    return whole*toRate+tail;
}
inline Frame scaleCeil(Frame frame,int64_t fromRate,int64_t toRate) {
    const auto floor=scaleFloor(frame,fromRate,toRate);
    const bool fractional=(frame%fromRate)*toRate%fromRate!=0;
    if(fractional&&floor==std::numeric_limits<Frame>::max())throw std::runtime_error("Source frame conversion overflow.");
    return floor+(fractional?1:0);
}
// SDL's five-channel format is 4.1. Explicit positions are padded to its
// eight-channel speaker bed so a source center never becomes an LFE channel.
struct DeviceLayout {
    int channels=0;
    std::vector<size_t> inputs;
    DeviceLayout(int64_t count,std::vector<std::string> speakers) {
        if(count<1||count>32)throw std::runtime_error("Invalid native channel count.");
        if(speakers.empty()&&count==1)speakers={"FC"};
        if(speakers.empty()&&count==2)speakers={"FL","FR"};
        if(speakers.size()!=static_cast<size_t>(count))
            throw std::runtime_error("Audition requires explicit multichannel speaker positions.");
        if((count==1&&speakers==std::vector<std::string>{"FC"})||
           (count==2&&speakers==std::vector<std::string>{"FL","FR"})) {
            channels=static_cast<int>(count);for(size_t i=0;i<speakers.size();++i)inputs.push_back(i);return;
        }
        static constexpr std::array<std::string_view,8> bed{"FL","FR","FC","LFE","BL","BR","SL","SR"};
        channels=8;
        for(const auto& speaker:speakers) {
            const auto found=std::find(bed.begin(),bed.end(),speaker);
            if(found==bed.end())throw std::runtime_error("Device audition does not support speaker position: "+speaker);
            const auto index=static_cast<size_t>(found-bed.begin());
            if(std::find(inputs.begin(),inputs.end(),index)!=inputs.end())throw std::runtime_error("Duplicate native speaker position.");
            inputs.push_back(index);
        }
    }
};
struct NativeTimeline {
    int64_t rate=0,channels=0;
    Frame frames=0;
    SampleType type=SampleType::Float32;
    void validate()const {
        if(rate<=0||rate>768000||channels<=0||channels>32||frames<=0)
            throw std::runtime_error("Invalid prepared audio timeline.");
        (void)sampleBytes(type);
    }
    double seconds(Frame frame)const {
        validate();if(frame<0||frame>frames)throw std::runtime_error("Position outside native timeline.");
        return static_cast<double>(frame)/static_cast<double>(rate);
    }
    Frame fromSeconds(double seconds)const {
        validate();const auto scaled=static_cast<long double>(seconds)*rate;
        if(!std::isfinite(seconds)||seconds<0||scaled>frames)throw std::runtime_error("Time outside native timeline.");
        return static_cast<Frame>(scaled);
    }
};
// Retain original payload bits. Conversion to doubles is only for UI/analysis;
// native region persistence/export continues to use native frame coordinates.
class Block {
    NativeTimeline timeline;
    Frame origin=0,count=0;
    std::vector<std::byte> data;
public:
    Block(NativeTimeline source,Frame start,Frame frames,std::span<const std::byte> payload)
        :timeline(source),origin(start),count(frames) {
        static_assert(std::endian::native==std::endian::little);
        timeline.validate();
        const auto stride=static_cast<size_t>(timeline.channels)*sampleBytes(timeline.type);
        if(origin<0||origin>timeline.frames||count<0||count>timeline.frames-origin||
           static_cast<uint64_t>(count)>32*1024*1024/stride||payload.size()!=static_cast<size_t>(count)*stride)
            throw std::runtime_error("Invalid or oversized prepared PCM block.");
        data.assign(payload.begin(),payload.end());
        if(timeline.type!=SampleType::Int32)
            for(Frame i=0;i<count;++i)for(int64_t channel=0;channel<timeline.channels;++channel)
                if(!std::isfinite(sample(i,channel)))throw std::runtime_error("Nonfinite prepared PCM.");
    }
    const auto& bytes()const{return data;}
    Frame start()const{return origin;}
    Frame frames()const{return count;}
    const NativeTimeline& source()const{return timeline;}
    double sample(Frame frame,int64_t channel)const {
        if(frame<0||frame>=count||channel<0||channel>=timeline.channels)throw std::runtime_error("PCM sample outside block.");
        const auto* pointer=data.data()+(static_cast<size_t>(frame)*static_cast<size_t>(timeline.channels)+static_cast<size_t>(channel))*sampleBytes(timeline.type);
        if(timeline.type==SampleType::Float64){double value;std::memcpy(&value,pointer,8);return value;}
        if(timeline.type==SampleType::Int32){int32_t value;std::memcpy(&value,pointer,4);return static_cast<double>(value)/2147483648.0;}
        float value;std::memcpy(&value,pointer,4);return value;
    }
};
}

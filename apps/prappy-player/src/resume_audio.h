#pragma once
#include "sample_client.h"
#include "state_io.h"
#include <bcrypt.h>

namespace sampling::resume_audio {
inline constexpr size_t Slots=4;
inline constexpr size_t BlockLimit=16*1024*1024;
inline constexpr size_t HeaderLimit=64*1024;

inline std::string digest(std::span<const std::byte> bytes){
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
        throw std::runtime_error("Cannot open resume audio integrity provider.");
    std::array<unsigned char,32> hash{};
    const auto status=BCryptHash(algorithm,nullptr,0,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),static_cast<ULONG>(bytes.size()),hash.data(),
        static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0)throw std::runtime_error("Cannot verify resume audio.");
    std::string hex;
    for(const auto byte:hash){hex+="0123456789abcdef"[byte>>4];hex+="0123456789abcdef"[byte&15];}
    return hex;
}

inline fs::path slot(const fs::path& directory,size_t index){
    return directory/(std::to_string(index)+".pcm");
}

inline void write(const fs::path& directory,const std::string& scope,const std::string& endpoint,
    const Json& descriptor,const prepared::Block& block,const std::string& epoch,std::stop_token stop){
    if(block.bytes().size()>BlockLimit||stop.stop_requested())return;
    fs::create_directories(directory);
    // Only this process's serialized cache writer creates these staging names.
    for(const auto& entry:fs::directory_iterator(directory)){
        const auto name=entry.path().filename().wstring();
        if(name.size()>10&&name[0]>=L'0'&&name[0]<L'0'+static_cast<wchar_t>(Slots)&&
           name.substr(1).starts_with(L".pcm.tmp-")){
            std::error_code ignored;fs::remove(entry.path(),ignored);
        }
    }
    if(fs::space(directory).available<1024ULL*1024*1024+block.bytes().size())return;
    const auto start=block.start(),count=block.frames();
    const auto metadata=Json{{"scope",scope},{"endpoint",endpoint},{"descriptor",descriptor},{"start",start},
        {"count",count},{"sha256",digest(block.bytes())},{"epoch",epoch}}.dump();
    if(metadata.size()>HeaderLimit||stop.stop_requested())return;
    const auto length=static_cast<uint32_t>(metadata.size());
    std::string record(reinterpret_cast<const char*>(&length),sizeof(length));
    record.reserve(sizeof(length)+metadata.size()+block.bytes().size());
    record+=metadata;
    record.append(reinterpret_cast<const char*>(block.bytes().data()),block.bytes().size());
    if(!stop.stop_requested())
        stateio::write(slot(directory,static_cast<size_t>(start/std::max<Frame>(1,block.source().rate*3))%Slots),record,false);
}

inline void read(const fs::path& directory,const std::string& scope,const std::string& endpoint,
    const Json& expected,const PcmBudget& budget,Reply& reply,std::stop_token stop){
    const bool descriptorOnly=expected.contains("capture_id");
    const auto wanted=expected.value("origin_frame",Frame{0});
    for(size_t index=0;index<Slots&&!stop.stop_requested();++index)try{
        std::ifstream input(slot(directory,index),std::ios::binary);
        uint32_t length=0;
        if(!input.read(reinterpret_cast<char*>(&length),sizeof(length))||!length||length>HeaderLimit)continue;
        std::string header(length,'\0');
        if(!input.read(header.data(),length))continue;
        auto meta=Json::parse(header);
        if(meta.at("scope")!=scope||meta.at("endpoint")!=endpoint)continue;
        const auto& descriptor=meta.at("descriptor");
        if(descriptorOnly){
            const auto& capture=descriptor.at("capture");
            if(capture.at("id")!=expected.at("capture_id")||capture.at("blob_sha256")!=expected.at("original"))continue;
            const auto start=meta.at("start").get<Frame>(),count=meta.at("count").get<Frame>();
            const auto rate=prepared::integer(descriptor.at("sample_rate"));
            const auto cue=std::round(expected.at("seconds").get<double>()*static_cast<double>(rate));
            if(start<0||count<=0||cue<static_cast<double>(start)||cue-static_cast<double>(start)>=static_cast<double>(count))continue;
            reply.body=descriptor;reply.status=200;return;
        }
        bool matches=true;
        for(const char* key:{"source_key","timeline_id","sample_rate","sample_format","channels"})
            if(descriptor.at(key)!=expected.at(key))matches=false;
        if(!matches)continue;
        const prepared::NativeTimeline clock{prepared::integer(descriptor.at("sample_rate")),
            prepared::integer(descriptor.at("channels")),prepared::integer(descriptor.at("frame_count")),
            prepared::sampleType(descriptor.at("sample_format").get<std::string>())};
        clock.validate();
        const auto start=prepared::integer(meta.at("start")),count=prepared::integer(meta.at("count"));
        const auto stride=static_cast<size_t>(clock.channels)*prepared::sampleBytes(clock.type);
        if(start<0||start>=clock.frames||count<=0||count>clock.frames-start||
           static_cast<uint64_t>(count)>BlockLimit/stride||wanted<start||wanted-start>=count)continue;
        const auto frames=std::min(prepared::integer(expected.at("frame_count")),count-(wanted-start));
        if(frames<=0)continue;
        const auto bytesNeeded=static_cast<size_t>(count)*stride;
        auto scratch=budget.reserve(bytesNeeded);
        std::vector<std::byte> bytes(bytesNeeded);
        if(!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))||
           digest(bytes)!=meta.at("sha256").get<std::string>()||stop.stop_requested())continue;
        reply.preparedPcm=std::make_shared<const prepared::Block>(clock,wanted,frames,
            std::span(bytes).subspan(static_cast<size_t>(wanted-start)*stride,static_cast<size_t>(frames)*stride));
        reply.body={{"decoder_epoch",meta.at("epoch")}};reply.status=200;return;
    }catch(const std::exception&){continue;}
}
}

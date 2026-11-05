#pragma once
#include "prepared_audio.h"
#include <json.hpp>
#include <optional>
#include <set>
#include <utility>

namespace prepared {
struct CatalogSource {
    SourceRef source;
    std::string asset,state,title,artist,codec,libraryName,originalPath;
    uint64_t originalBytes=0;
    double duration=0;
    NativeTimeline native,listening;
    std::optional<double> loudness,peak;
    std::vector<std::pair<std::string,std::string>> tags;
    bool ready()const{return state=="Ready";}
};
inline int64_t integer(const nlohmann::json& value) {
    if(!value.is_number_integer()||
       (value.is_number_unsigned()&&value.get<uint64_t>()>static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
        throw std::runtime_error("Prepared catalog expected a bounded integer.");
    return value.get<int64_t>();
}
inline std::vector<CatalogSource> parseCatalog(const nlohmann::json& snapshot) {
    const auto& items=snapshot.at("items");
    if(!items.is_array()||items.size()>1000000)throw std::runtime_error("Prepared catalog exceeds source limit.");
    std::set<std::pair<std::string,std::string>> seen;
    std::vector<CatalogSource> result;
    result.reserve(items.size());
    for(const auto& row:items) {
        CatalogSource source;
        source.source={row.at("library_id"),row.at("capture_id")};source.source.validate();
        if(!seen.emplace(source.source.library,source.source.capture).second)throw std::runtime_error("Duplicate prepared catalog source.");
        source.asset=row.at("asset_key");
        if(source.asset.size()!=64||source.asset.find_first_not_of("0123456789abcdef")!=std::string::npos)
            throw std::runtime_error("Invalid prepared asset identity.");
        source.state=row.at("state");
        if(source.state!="Queued"&&source.state!="Preparing"&&source.state!="Ready"&&source.state!="Failed"&&
           source.state!="Offline"&&source.state!="Stale"&&source.state!="CapacityBlocked")
            throw std::runtime_error("Unknown preparation state.");
        source.title=row.at("title");
        source.originalPath=row.at("original_path");
        if(!source.originalPath.starts_with('/')||source.originalPath.find('\0')!=std::string::npos)
            throw std::runtime_error("Invalid original recording location.");
        source.artist=row.at("artist").is_null()?std::string{}:row.at("artist").get<std::string>();
        source.codec=row.at("codec");source.libraryName=row.at("library_name");
        const auto bytes=integer(row.at("original_bytes"));
        if(bytes<1)throw std::runtime_error("Invalid original byte count.");
        source.originalBytes=static_cast<uint64_t>(bytes);
        source.duration=row.at("duration_seconds");
        if(!std::isfinite(source.duration)||source.duration<=0)throw std::runtime_error("Invalid source duration.");
        if(source.ready()) {
            const auto& analysis=row.at("analysis");
            source.native={integer(analysis.at("native_rate")),integer(analysis.at("native_channels")),
                integer(analysis.at("native_frames")),sampleType(analysis.at("native_sample_format").get<std::string>())};
            source.listening={integer(analysis.at("sample_rate")),integer(analysis.at("channels")),
                integer(analysis.at("listening_frames")),SampleType::Float32};
            source.native.validate();source.listening.validate();
            if(source.listening.rate!=48000||source.listening.channels!=2)
                throw std::runtime_error("Invalid listening render profile.");
            if(!analysis.at("loudness_lufs").is_null()) {
                source.loudness=analysis.at("loudness_lufs").get<double>();
                if(!std::isfinite(*source.loudness)||*source.loudness< -70||*source.loudness>50)
                    throw std::runtime_error("Invalid prepared loudness.");
            }
            source.peak=analysis.at("sample_peak").get<double>();
            if(!std::isfinite(*source.peak)||*source.peak<0||*source.peak>64)
                throw std::runtime_error("Invalid prepared sample peak.");
            const auto& tags=analysis.at("source_tags");
            if(!tags.is_object())throw std::runtime_error("Invalid original gain tags.");
            for(const auto& [name,value]:tags.items()) {
                if(!value.is_string())throw std::runtime_error("Invalid original gain tag value.");
                source.tags.emplace_back(name,value.get<std::string>());
            }
        }
        result.push_back(std::move(source));
    }
    return result;
}
}

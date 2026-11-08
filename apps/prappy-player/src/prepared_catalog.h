#pragma once
#include "prepared_audio.h"
#include <json.hpp>
#include <optional>
#include <set>
#include <utility>

namespace prepared {
struct NotReady:std::runtime_error {using std::runtime_error::runtime_error;};
struct CatalogSource {
    SourceRef source;
    std::string asset,timeline,state,title,artist,codec,libraryName,originalPath;
    uint64_t originalBytes=0;
    double duration=0;
    NativeTimeline native,listening;
    std::optional<double> loudness,peak;
    std::vector<std::pair<std::string,std::string>> tags;
    bool ready()const{return state=="Playable";}
    void analysis(const nlohmann::json& analysis){
        loudness.reset();peak.reset();
        if(analysis.contains("source_tags")){
            const auto& originalTags=analysis.at("source_tags");
            if(!originalTags.is_object())throw std::runtime_error("Invalid original gain tags.");
            tags.clear();
            for(const auto& [name,value]:originalTags.items()){
                if(!value.is_string())throw std::runtime_error("Invalid original gain tag.");
                tags.emplace_back(name,value.get<std::string>());
            }
        }
        if(analysis.value("state",std::string("Ready"))!="Ready")return;
        if(analysis.contains("loudness_lufs")&&!analysis.at("loudness_lufs").is_null())loudness=analysis.at("loudness_lufs").get<double>();
        if(analysis.contains("sample_peak")&&!analysis.at("sample_peak").is_null())peak=analysis.at("sample_peak").get<double>();
        if((loudness&&!std::isfinite(*loudness))||
           (peak&&(!std::isfinite(*peak)||*peak<0)))throw std::runtime_error("Invalid source analysis.");
    }
};
inline int64_t integer(const nlohmann::json& value) {
    if(!value.is_number_integer()||
       (value.is_number_unsigned()&&value.get<uint64_t>()>static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
        throw std::runtime_error("Prepared catalog expected a bounded integer.");
    return value.get<int64_t>();
}
inline std::vector<CatalogSource> parseCatalog(const nlohmann::json& snapshot) {
    if(snapshot.value("protocol",0)!=3)throw std::runtime_error("Indexed audio requires backend protocol 3.");
    const auto& items=snapshot.at("items");
    if(!items.is_array()||items.size()>1000000)throw std::runtime_error("Prepared catalog exceeds source limit.");
    std::set<std::pair<std::string,std::string>> seen;
    std::vector<CatalogSource> result;
    result.reserve(items.size());
    for(const auto& row:items) {
        CatalogSource source;
        source.source={row.at("library_id"),row.at("capture_id")};source.source.validate();
        if(!seen.emplace(source.source.library,source.source.capture).second)throw std::runtime_error("Duplicate prepared catalog source.");
        source.asset=row.at("original_sha256");
        if(source.asset.size()!=64||source.asset.find_first_not_of("0123456789abcdef")!=std::string::npos)
            throw std::runtime_error("Invalid original source hash.");
        source.state=row.at("state");
        if(source.state!="Playable"&&source.state!="Offline"&&source.state!="Failed")
            throw std::runtime_error("Unknown indexed source state.");
        source.title=row.at("title");source.libraryName=row.at("library_name");
        source.originalPath=row.at("original_path");
        if((source.ready()&&!source.originalPath.starts_with('/'))||source.originalPath.find('\0')!=std::string::npos)
            throw std::runtime_error("Invalid original recording location.");
        source.artist=row.at("artist").is_null()?std::string{}:row.at("artist").get<std::string>();
        const auto bytes=integer(row.at("original_bytes"));
        if(bytes<1)throw std::runtime_error("Invalid original byte count.");
        source.originalBytes=static_cast<uint64_t>(bytes);
        const auto& media=row.at("media");source.codec=media.at("codec");
        source.duration=media.at("duration_seconds");
        source.native.rate=integer(media.at("sample_rate"));source.native.channels=integer(media.at("channels"));
        // Catalog duration is display-only. Exact frames/type arrive in the
        // bounded source descriptor when editing starts, independently of analysis.
        if(!std::isfinite(source.duration)||source.duration<=0||source.native.rate<1||source.native.rate>768000||
           source.native.channels<1||source.native.channels>32)throw std::runtime_error("Invalid source media dimensions.");
        if(row.contains("analysis"))source.analysis(row.at("analysis"));
        result.push_back(std::move(source));
    }
    return result;
}
}

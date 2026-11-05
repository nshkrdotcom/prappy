#pragma once
#include "prepared_audio.h"
#include <json.hpp>

namespace prepared {
// Request context is checked against every returned binary header and payload.
inline nlohmann::json pcmContext(const nlohmann::json& manifest,Frame start,Frame count,bool listening=false) {
    const auto& domain=listening?manifest.at("listening"):manifest;
    NativeTimeline timeline{domain.at("sample_rate"),domain.at("channels"),domain.at("frames"),
        sampleType(domain.at("sample_format").get<std::string>())};
    timeline.validate();
    if(start<0||count<1||start>=timeline.frames||count>timeline.frames-start||
       static_cast<uint64_t>(count)>64*1024*1024/(timeline.channels*sampleBytes(timeline.type)))
        throw std::runtime_error("Prepared request exceeds source bounds or memory budget.");
    return {{"asset_key",manifest.at("key")},{"origin_frame",start},{"frame_count",count},
        {"sample_rate",timeline.rate},{"channels",timeline.channels},{"sample_format",domain.at("sample_format")},
        {"total_frames",timeline.frames},{"domain",listening?"listening":"native"}};
}
}

#pragma once
#include "prepared_catalog.h"
#include <json.hpp>

namespace prepared {
// Request context is checked against every returned binary header and payload.
inline nlohmann::json pcmContext(const nlohmann::json& manifest,Frame start,Frame count) {
    const auto& domain=manifest;
    NativeTimeline timeline{integer(domain.at("sample_rate")),integer(domain.at("channels")),integer(domain.at("frame_count")),
        sampleType(domain.at("sample_format").get<std::string>())};
    timeline.validate();
    if(start<0||count<1||start>=timeline.frames||count>timeline.frames-start||
       static_cast<uint64_t>(count)>32*1024*1024/(timeline.channels*sampleBytes(timeline.type)))
        throw std::runtime_error("Prepared request exceeds source bounds or memory budget.");
    return {{"source_key",manifest.at("source_key")},{"timeline_id",manifest.at("timeline_id")},{"origin_frame",start},{"frame_count",count},
        {"sample_rate",timeline.rate},{"channels",timeline.channels},{"sample_format",domain.at("sample_format")},
        {"total_frames",timeline.frames},{"domain","native"}};
}
}

#include "remote_waveform.h"

void RemoteWaveform::select(const prepared::CatalogSource& selected,const sampling::Connection& backend,bool force) {
    const auto next=selected.source.library+"/"+selected.source.capture+"/"+selected.asset+"/"+selected.state;
    if(!force&&identity==next)return;
    identity=next;source=selected;++generation;waveform.reset();pollAt={};message=source.state;
    if(!source.ready())return;
    if(!client||connection.url!=backend.url||connection.token!=backend.token){
        connection=backend;client=std::make_unique<sampling::Client>(connection,notify);
    }
    try {
        client->submit({"source-descriptor","GET",source.source.endpoint(),nullptr,generation,{}});
        message="Reading waveform";
    } catch(const std::exception& error) {
        // The source is still selected; a saturated executor must not make
        // this identity permanently unrequestable until the user switches away.
        message=error.what();pollAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    }
}

bool RemoteWaveform::collect() {
    if(!client)return false;
    if(pollAt!=std::chrono::steady_clock::time_point{}&&std::chrono::steady_clock::now()>=pollAt){
        pollAt={};
        try {client->submit({"source-descriptor","GET",source.source.endpoint(),nullptr,generation,{}});}
        catch(const std::exception& error){
            message=error.what();pollAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        }
    }
    bool changed=false;
    for(auto& reply:client->collect()){
        if(reply.request.generation!=generation)continue;
        changed=true;
        try{
            if(!reply.error.empty())throw std::runtime_error(reply.error);
            if(reply.request.tag=="source-descriptor"){
                if(reply.body.at("capture").at("id")!=source.source.capture||reply.body.at("capture").at("blob_sha256")!=source.asset)
                    throw std::runtime_error("Waveform source identity changed.");
                source.timeline=reply.body.at("timeline_id");
                if(reply.body.contains("analysis"))source.analysis(reply.body.at("analysis"));
                source.native={prepared::integer(reply.body.at("sample_rate")),prepared::integer(reply.body.at("channels")),
                    prepared::integer(reply.body.at("frame_count")),prepared::sampleType(reply.body.at("sample_format").get<std::string>())};
                source.native.validate();
                int level=0;prepared::Frame stride=4096;
                while(prepared::ceilDivPositive(source.native.frames,stride)>2048){++level;stride*=2;}
                if(!reply.body.contains("waveform")&&source.native.frames>source.native.rate*6){
                    message="Waveform analysis pending";pollAt=std::chrono::steady_clock::now()+std::chrono::seconds(5);continue;
                }
                const auto count=prepared::ceilDivPositive(source.native.frames,stride);
                const auto key=reply.body.at("source_key").get<std::string>();
                sampling::Request request;request.tag="source-waveform";request.generation=generation;
                request.path=source.source.endpoint()+"/waveform?source_key="+key+"&timeline="+source.timeline+"&level="+
                    std::to_string(level)+"&start=0&count="+std::to_string(count);
                request.body={{"source_key",key},{"timeline_id",source.timeline},{"channels",source.native.channels},
                    {"start_bin",0},{"bin_count",count},{"frames_per_bin",stride}};
                client->submit(std::move(request));
            }else if(reply.request.tag=="source-waveform"){
                const auto channels=static_cast<size_t>(source.native.channels),step=channels*2;
                if(reply.pcm.empty()||reply.pcm.size()%step)throw std::runtime_error("Invalid native waveform bins.");
                auto result=std::make_shared<analysis::Result>();result->frames=static_cast<uint64_t>(source.native.frames);
                result->sampleRate=static_cast<uint64_t>(source.native.rate);result->loudness=source.loudness;result->levels.emplace_back();
                auto& peaks=result->levels.back();peaks.reserve(reply.pcm.size()/step);
                for(size_t i=0;i<reply.pcm.size();i+=step){
                    float low=reply.pcm[i],high=reply.pcm[i+1],rightLow=low,rightHigh=high;
                    if(channels==2){rightLow=reply.pcm[i+2];rightHigh=reply.pcm[i+3];}
                    else for(size_t channel=1;channel<channels;++channel){low=std::min(low,reply.pcm[i+channel*2]);high=std::max(high,reply.pcm[i+channel*2+1]);rightLow=low;rightHigh=high;}
                    if(low>high||rightLow>rightHigh)throw std::runtime_error("Waveform extrema are reversed.");
                    peaks.push_back({low,high,rightLow,rightHigh});
                }
                waveform=std::move(result);message="Ready";
            }
        }catch(const std::exception& error){
            message=error.what();
            if(std::string_view(error.what()).find("request/reply queue is full")!=std::string_view::npos)
                pollAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        }
    }
    return changed;
}

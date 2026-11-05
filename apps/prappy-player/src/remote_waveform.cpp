#include "remote_waveform.h"

void RemoteWaveform::select(const Track& track,const sampling::Connection& backend,bool force) {
    if(!track.prepared){clear();return;}
    const auto& selected=*track.prepared;
    const auto next=selected.source.library+"/"+selected.source.capture+"/"+selected.asset+"/"+selected.state;
    if(!force&&identity==next)return;
    identity=next;source=selected;++generation;waveform.reset();message=source.state;
    if(!source.ready())return;
    if(!client||connection.url!=backend.url||connection.token!=backend.token) {
        connection=backend;client=std::make_unique<sampling::Client>(connection,notify);
    }
    sampling::Request request;request.tag="prepared-manifest";request.path=source.source.endpoint();
    request.generation=generation;client->submit(std::move(request));message="Reading prepared waveform";
}

bool RemoteWaveform::collect() {
    if(!client)return false;
    bool changed=false;
    for(auto& reply:client->collect()) {
        if(reply.request.generation!=generation)continue;
        changed=true;
        try {
            if(!reply.error.empty())throw std::runtime_error(reply.error);
            if(reply.status==202){message=reply.body.at("state");continue;}
            if(reply.request.tag=="prepared-manifest") {
                if(reply.body.at("key")!=source.asset||reply.body.at("state")!="Ready")
                    throw std::runtime_error("Prepared waveform source changed.");
                const auto& listening=reply.body.at("listening");
                if(prepared::integer(listening.at("frames"))!=source.listening.frames||
                   prepared::integer(listening.at("sample_rate"))!=48000||prepared::integer(listening.at("channels"))!=2)
                    throw std::runtime_error("Prepared listening timeline changed.");
                const auto& pyramid=listening.at("waveform");
                const auto& levels=pyramid.at("levels");
                if(!levels.is_array()||levels.empty()||levels.size()>48)
                    throw std::runtime_error("Invalid prepared waveform pyramid.");
                const auto level=levels.size()-1;
                const auto count=prepared::integer(levels.at(level).at("bins"));
                if(prepared::integer(pyramid.at("frames_per_bin"))!=256||prepared::integer(pyramid.at("channels"))!=2)
                    throw std::runtime_error("Unexpected prepared waveform format.");
                const auto stride=int64_t{256}*(int64_t{1}<<level);
                if(stride<=0||count<=0||count>8192||count!=source.listening.frames/stride+(source.listening.frames%stride?1:0))
                    throw std::runtime_error("Prepared waveform bins differ from the listening timeline.");
                sampling::Request request;request.tag="prepared-waveform";request.generation=generation;
                request.path=source.source.endpoint()+"/waveform?asset="+source.asset+"&domain=listening&level="+
                    std::to_string(level)+"&start=0&count="+std::to_string(count);
                request.body={{"asset_key",source.asset},{"channels",2},{"start_bin",0},
                              {"bin_count",count},{"frames_per_bin",stride}};
                client->submit(std::move(request));
            } else if(reply.request.tag=="prepared-waveform") {
                if(reply.pcm.empty()||reply.pcm.size()%4)throw std::runtime_error("Invalid prepared stereo peaks.");
                auto result=std::make_shared<analysis::Result>();
                result->frames=static_cast<uint64_t>(source.listening.frames);result->loudness=source.loudness;
                result->levels.emplace_back();auto& peaks=result->levels.back();peaks.reserve(reply.pcm.size()/4);
                for(size_t i=0;i<reply.pcm.size();i+=4) {
                    if(reply.pcm[i]>reply.pcm[i+1]||reply.pcm[i+2]>reply.pcm[i+3])
                        throw std::runtime_error("Prepared peak extrema are reversed.");
                    peaks.push_back({reply.pcm[i],reply.pcm[i+1],reply.pcm[i+2],reply.pcm[i+3]});
                }
                waveform=std::move(result);message="Ready";
            }
        } catch(const std::exception& error){message=error.what();}
    }
    return changed;
}

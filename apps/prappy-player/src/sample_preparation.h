#pragma once
#include "sample_client.h"
#include "sample_editor.h"
#include <chrono>

namespace sampling {
// One bounded speculative working set. Never calls Workspace::mark/openCapture or publishes commands.
class SourcePreparation {
    std::unique_ptr<Client> client;
    uint64_t generation=0,loadedSerial=0;
    bool connected=false,issued=false,connecting=false;
    int attempts=0;
    std::string configuredUrl,configuredToken;
    std::chrono::steady_clock::time_point due{},nextWindow{},retry{};
    void window(double position){
        const auto total=match.at("capture").at("timeline").at("frame_count").get<Frame>();
        const auto start=std::clamp<Frame>(static_cast<Frame>(position*Rate)-Rate,0,total-1),end=std::min(total,start+Rate*8);
        const std::string base="/v1/libraries/"+match.at("library_id").get<std::string>()+"/captures/"+match.at("capture").at("id").get<std::string>();
        client->submit({"pcm","GET",base+"/pcm?cached_only=true&start="+std::to_string(start)+"&end="+std::to_string(end)+"&timeline="+match.at("capture").at("timeline_id").get<std::string>(),nullptr,generation,{}});
        nextWindow=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    }
public:
    std::string path,status="Editor preparation idle",digest;
    Json match,overview,regions;
    std::vector<float> pcm;
    Frame pcmStart=0;
    std::string knownDigest(std::string_view current,uint64_t serial)const{
        return path==current&&loadedSerial==serial?digest:std::string{};
    }
    void tick(const Connection& connection,const std::function<void()>& notify,const std::string& current,double position,double duration,uint64_t serial){
        const auto now=std::chrono::steady_clock::now();
        const auto token=connection.credentialFile.generic_string();
        if(configuredUrl!=connection.url||configuredToken!=token){
            client.reset();connected=connecting=issued=false;attempts=0;path.clear();configuredUrl=connection.url;configuredToken=token;
        }
        if(current!=path||loadedSerial!=serial){path=current;loadedSerial=serial;++generation;issued=false;attempts=0;digest.clear();match=nullptr;overview=nullptr;regions=nullptr;pcm.clear();status="Preparing editor...";due=now+std::chrono::milliseconds(750);}
        if(path.empty()||duration<=0)return;
        if(!client&&now>=due)client=std::make_unique<Client>(connection,notify);
        if(!client)return;
        if(!connected&&!connecting&&attempts<3&&now>=retry){connecting=true;++attempts;client->submit({"libraries","GET","/v1/libraries",nullptr,generation,{}});}
        if(connected&&!issued&&now>=due){issued=true;client->submit({"hash","GET",path,nullptr,generation,{}});}
        if(!match.is_null()&&now>=nextWindow&&(pcm.empty()||position*Rate<pcmStart||position*Rate>pcmStart+static_cast<Frame>(pcm.size()/2)-Rate*2))window(position);
        for(auto& reply:client->collect()){
            if(reply.request.tag=="libraries"){
                connecting=false;connected=reply.error.empty();if(!connected){retry=now+std::chrono::seconds(10);status="Editor unavailable: "+reply.error;}continue;
            }
            if(reply.request.generation!=generation)continue;
            if(!reply.error.empty()){status="Editor preparation: "+reply.error;continue;}
            const auto& tag=reply.request.tag;
            if(tag=="hash"){
                digest=reply.body.at("sha256");client->submit({"resolve","GET","/v1/resolve?sha256="+digest,nullptr,generation,{}});
            }else if(tag=="resolve"){
                const auto& matches=reply.body.at("matches");
                if(matches.size()!=1){status=matches.empty()?"Song is not registered for editing":"Choose a source in the editor";continue;}
                match=matches[0];status="Source ready; warming cached audio...";
                const std::string base="/v1/libraries/"+match.at("library_id").get<std::string>()+"/captures/"+match.at("capture").at("id").get<std::string>();
                window(position);
                client->submit({"sourceRegions","GET",base+"/regions?timeline="+match.at("capture").at("timeline_id").get<std::string>()+"&limit=100&offset=0",nullptr,generation,{}});
                client->submit({"overview","GET",base+"/overview?cached_only=true",nullptr,generation,{}});
            }else if(tag=="pcm"){
                if(reply.status==202){status="Source ready; audio prepared on demand";continue;}
                pcmStart=reply.body.at("origin_frame");pcm=std::move(reply.pcm);status="Editor ready (cached audio)";
            }else if(tag=="overview"&&reply.status!=202)overview=reply.body;
            else if(tag=="sourceRegions")regions=reply.body;
        }
    }
};
}

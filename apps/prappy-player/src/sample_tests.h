#pragma once
#include "sample_editor.h"
#include "sample_audition.h"
#include "state_io.h"
#include "waveform_amplitude.h"
#include <fstream>
namespace sampling {
inline int integrationTest(const fs::path& manifestPath,const fs::path& logPath){
    std::ofstream log(logPath);
    const auto require=[](bool v,const char* m){if(!v)throw std::runtime_error(m);};
    try{
        AmplitudeScale scale;require(scale.gain==1 && scale.project(0,false)==1 && scale.project(0,true)==-1,"Actual display reference failed");
        scale.fit(.01,true);require(scale.mode==AmplitudeMode::Visible && std::abs(scale.gain-100)<1e-9,"Quiet visible fit failed");
        require(std::abs((scale.project(.01,false)-1)+(scale.project(-.01,true)+1))<1e-9,"Linked stereo scaling failed");
        scale.drag(scale.gain,80);require(scale.mode==AmplitudeMode::Manual && std::abs(scale.gain-200)<1e-9,"Manual ruler scale failed");
        scale.fit(0,false);require(scale.gain==1,"Silence must not amplify noise indefinitely");
        scale.fit(1e-5,false);require(scale.gain==AmplitudeScale::MaxGain,"Quiet fit bound failed");
        scale.fit(2,true);require(scale.gain==.5,"Above-full-scale fit failed");
        scale.actual();require(scale.gain==1 && scale.mode==AmplitudeMode::Actual,"Display reset failed");
        require(scale.project(100,false)<2 && scale.project(-100,true)>-2,"Display must stay inside channel lanes");
        StereoEnvelope envelope,projected;std::vector<float> quiet(2000,.01f);quiet[1999]=-.5f;
        require(envelope.fromPcm(quiet,5000,5000,6000,100) && envelope.x.size()<=100 && envelope.peak==.5,"Envelope frame bounds or peak failed");
        const auto original=quiet;scale.fit(envelope.peak,false);envelope.project(scale,projected);require(quiet==original && envelope.lmin[0]<.02,"Display mutated source PCM");
        require(!envelope.fromPcm(quiet,5000,4999,6000,100),"Partial PCM cannot define visible fit");
        const std::vector<PeakBin> bins={{0,100,-.1,.1,-.2,.2},{100,200,-.5,.5,-.75,.75}};
        envelope.fromOverview(bins,100,101);require(envelope.x.size()==1 && envelope.peak==.75,"Overview range intersection failed");
        log<<"PASS visual amplitude: linked fit, silence/bounds, manual drag, reset, lane clipping and immutable PCM/envelopes\n";
        Editor e;e.reset(48000,{1000,2000});e.change({1001,1999});e.undo();require(e.span==Span{1000,2000},"Undo failed");e.redo();require(e.span==Span{1001,1999},"Redo failed");e.change({-100,999999});require(e.span==Span{0,48000},"Frame clamps failed");e.change({47999,48000});require(e.span.end-e.span.start==1,"One-frame EOF failed");
        require(e.snap(1003,{1000,2000},5)==1000 && e.snap(1010,{1000},5)==1010,"Snap tolerance failed");
        e.reset(48000,{1000,2000});e.beginGesture();e.change({1100,2000});e.change({1200,2000});e.endGesture();e.undo();require(e.span==Span{1000,2000},"Drag gesture must undo as one edit");
        log<<"PASS integer frames, undo/redo, one-frame EOF, bounds and snapping\n";
        const auto manifest=Json::parse(stateio::read(manifestPath));
        const auto tokenPath=manifest.at("token_file").get<std::string>();
        Connection connection{manifest.at("url"),trim(stateio::read(fs::path(std::u8string(reinterpret_cast<const char8_t*>(tokenPath.data()),tokenPath.size()))))};
        const auto base="/v1/libraries/"+manifest.at("library_id").get<std::string>();
        const auto request=[&](std::string tag,std::string method,std::string path,Json body=nullptr){
            auto reply=perform(connection,{tag,method,path,body,0,{}});
            if(!reply.error.empty())throw std::runtime_error(reply.error);
            return reply;
        };
        auto libraries=request("libraries","GET","/v1/libraries").body;
        require(!libraries.empty(),"No HTTP libraries");
        auto rows=request("browse","GET",base+"/recordings").body.at("items");require(!rows.empty(),"No recordings");
        const auto capture=rows[0];const auto cap=capture.at("id").get<std::string>();
        const auto timeline=capture.at("timeline_id").get<std::string>();
        auto pcm=request("pcm","GET",base+"/captures/"+cap+"/pcm?start=1000&end=2000&timeline="+timeline).pcm;
        require(pcm.size()==2000,"PCM exact frame count failed");
        require(std::abs(pcm[0]-.5f)<.000001f && std::abs(pcm[1998]+.5f)<.000001f,"PCM sample boundaries failed");
        log<<"PASS native Windows HTTP authentication, catalog and exact canonical PCM\n";
        const auto cmd=[&](std::string operation,Json payload,std::string id=""){
            if(id.empty())id=uuid();
            return request("command","POST",base+"/commands",{{"request_id",id},{"operation",operation},{"payload",payload}}).body;
        };
        const auto commandId=uuid();
        Json payload={{"capture_id",cap},{"name","Native exact splice"},{"kind","one_shot"},{"span",{{"start_frame",1000},{"end_frame",2000}}},{"facts",Json::array({{{"facet","tags"},{"value",Json::array({"native-test","hit"})},{"origin","human"},{"state","accepted"}}})}};
        const auto first=cmd("region.save",payload,commandId);const auto region=first.at("region");const auto id=region.at("id").get<std::string>();
        payload["id"]=id;payload["expected"]=1;payload["span"]["start_frame"]=1001;
        require(cmd("region.save",payload).at("region").at("revision")==2,"Region edit revision failed");
        payload.erase("id");payload.erase("expected");payload["span"]["start_frame"]=1000;
        require(cmd("region.save",payload,commandId).at("region").at("revision")==1,"Exact replay after later edit failed");
        auto samples=request("browse","GET",base+"/regions?q=native-test").body.at("items");require(!samples.empty(),"Sample metadata search failed");
        auto collection=cmd("collection.create",{{"name","Native sample collection"}}).at("collection");
        cmd("collection.add",{{"id",collection.at("id")},{"expected",1},{"region",{{"id",id},{"revision",1}}}});
        auto prepared=request("export","POST",base+"/regions/"+id+"/exports/1",Json::object()).body;
        require(prepared.at("variant").at("frame_count")==1000,"Export frame count failed");
        const auto destination=logPath.parent_path()/"native-export.flac";
        std::error_code ec;fs::remove(destination,ec);
        auto downloaded=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",prepared.at("variant").at("audio_sha256")}},0,destination});
        require(downloaded.error.empty(),"Artifact verification/download failed");
        downloaded=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",prepared.at("variant").at("audio_sha256")}},0,destination});
        require(downloaded.error.empty(),"Repeated verified export failed");
        auto corrupt=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",std::string(64,'0')}},0,logPath.parent_path()/"rejected.flac"});
        require(!corrupt.error.empty() && !fs::exists(logPath.parent_path()/"rejected.flac.partial"),"Corrupt download must be removed");
        std::stop_source stopped;stopped.request_stop();require(!perform(connection,{"libraries","GET","/v1/libraries",nullptr,0,{}},stopped.get_token()).error.empty(),"Cancelled request should stop");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
        Audition preview;preview.play(std::move(pcm),true);SDL_Delay(100);preview.stop();
        log<<"PASS native save/edit/replay, metadata search, collection, verified export and SDL loop lifetime\nALL PASS\n";
        return 0;
    }catch(const std::exception& ex){log<<"FAIL "<<ex.what()<<"\n";return 1;}
}
}

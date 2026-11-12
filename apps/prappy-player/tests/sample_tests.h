#pragma once
#include "sample_editor.h"
#include "sample_client.h"
#include "prepared_requests.h"
#include "sample_audition.h"
#include "state_io.h"
#include "waveform_amplitude.h"
#include "waveform_viewport.h"
#include <fstream>
namespace sampling {
inline int integrationTest(const fs::path& manifestPath,const fs::path& logPath){
    std::ofstream log(logPath);
    const auto require=[](bool v,const char* m){if(!v)throw std::runtime_error(m);};
    try{
        EditorTransport transport;require(transport.loop&&transport.scope==PlaybackScope::Selection,"Editor must default to looped selection");
        require(transport.bounds({1000,2000},100000)==Span{1000,2000},"Selection playback bounds failed");
        transport.scope=PlaybackScope::Context;require(transport.bounds({1000,2000},100000)==Span{0,50000},"Context lead-in/tail bounds failed");
        transport.scope=PlaybackScope::Recording;require(transport.bounds({1000,2000},Rate*900)==Span{0,Rate*900},"Recording transport must support long sources");
        transport.boundary=BoundaryCheck::End;require(!transport.effectiveLoop()&&transport.bounds({1000,2000},100000)==Span{0,14000},"Boundary check must be explicit and one-shot");
        transport.range={1000,2000};transport.cursor=1500;transport.stop();require(transport.cursor==1000&&transport.state==PlaybackState::Stopped,"Stop must reset to playback start");
        log<<"PASS editor transport: default loop, selection/context/long-recording scopes, boundary checks and stop\n";
        const auto fractional=waveformRange(945674.152727865,992634.595901516,29728601);
        require(fractional==WaveformRange{945674,992635},"Fractional viewport must request its last intersecting sample");
        StereoEnvelope fractionalEnvelope;const std::vector<float> fractionalPcm(static_cast<size_t>(fractional.end-fractional.start)*2,.01f);
        require(fractionalEnvelope.fromPcm(fractionalPcm,fractional.start,fractional.start,fractional.end,800),"Fractional viewport must render exact PCM rather than overview");
        require(waveformRange(9.2,10.8,10)==WaveformRange{9,10} && waveformRange(-1.,.2,10)==WaveformRange{0,1},"Viewport bounds at recording edges failed");
        require(waveformRange(1000.,2000.,10000)==WaveformRange{1000,2000},"Integer viewport must not request extra samples");
        require(waveformRange(1000.,2000.,10000)!=waveformRange(1000.,2000.1,10000),"Sub-frame navigation across an integer boundary must refresh PCM");
        log<<"PASS fractional waveform viewport: request/render coverage, integer bounds and one-frame EOF\n";
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
        const auto cap=manifest.at("capture_id").get<std::string>(),endpoint=base+"/captures/"+cap+"/audio";
        const auto source=request("source-descriptor","GET",endpoint).body;
        const auto timeline=source.at("timeline_id").get<std::string>(),sourceKey=source.at("source_key").get<std::string>();
        auto decoded=request("source-pcm","GET",endpoint+"/pcm?start=1000&count=1000&timeline="+timeline+"&source_key="+sourceKey,prepared::pcmContext(source,1000,1000));
        require(decoded.preparedPcm&&decoded.preparedPcm->start()==1000&&decoded.preparedPcm->frames()==1000,"Typed PCM exact frame count failed");
        PcmBudget uploadMemory;NativeSession clips(decoded.preparedPcm->source(),sourceKey,timeline,1,uploadMemory);
        clips.adopt(decoded.preparedPcm,uploadMemory.reserve(decoded.preparedPcm->bytes().size()),sourceKey,timeline,1,decoded.body.at("decoder_epoch"));
        const auto save=[&](const Json& payload,std::string id=""){
            Request hash{"native-save-hash","LOCAL","",Json::object(),0,{}};
            hash.upload=clips.clip(payload.at("span").at("start_frame"),payload.at("span").at("end_frame"));
            const auto fingerprint=perform(connection,hash);require(fingerprint.error.empty(),"Native save hash failed");
            Request upload{"command","POST",base+"/native-saves",{{"request_id",id.empty()?uuid():id},{"operation","region.save"},
                {"payload",payload},{"source_key",sourceKey},{"pcm_sha256",fingerprint.body.at("sha256")},{"decoder_epoch",hash.upload->decoderEpoch}},0,{}};
            upload.upload=hash.upload;auto reply=perform(connection,upload);
            if(!reply.error.empty())throw std::runtime_error(reply.error);return reply.body;
        };
        log<<"PASS native Windows HTTP authentication, catalog and typed native PCM\n";
        const auto cmd=[&](std::string operation,Json payload,std::string id=""){
            if(id.empty())id=uuid();
            return request("command","POST",base+"/commands",{{"request_id",id},{"operation",operation},{"payload",payload}}).body;
        };
        const auto commandId=uuid();
        Json payload={{"capture_id",cap},{"timeline_id",timeline},{"name","Native exact splice"},{"kind","one_shot"},{"span",{{"start_frame",1000},{"end_frame",2000}}},{"facts",Json::array({{{"facet","tags"},{"value",Json::array({"native-test","hit"})},{"origin","human"},{"state","accepted"}}})}};
        const auto first=save(payload,commandId);const auto region=first.at("region");const auto id=region.at("id").get<std::string>();
        payload["id"]=id;payload["expected"]=1;payload["span"]["start_frame"]=1001;
        require(save(payload).at("region").at("revision")==2,"Region edit revision failed");
        payload.erase("id");payload.erase("expected");payload["span"]["start_frame"]=1000;
        require(save(payload,commandId).at("region").at("revision")==1,"Exact replay after later edit failed");
        auto samples=request("browse","GET",base+"/regions?q=native-test").body.at("items");require(!samples.empty(),"Sample metadata search failed");
        auto collection=cmd("collection.create",{{"name","Native sample collection"}}).at("collection");
        cmd("collection.add",{{"id",collection.at("id")},{"expected",1},{"region",{{"id",id},{"revision",1}}}});
        auto prepared=request("export","POST",base+"/regions/"+id+"/exports/1",Json::object()).body;
        require(prepared.at("variant").at("frame_count")==1000,"Export frame count failed");
        const auto frozenEndpoint=base+"/regions/"+id+"/revisions/1/audio";
        const auto frozen=request("source-descriptor","GET",frozenEndpoint).body;
        auto reopened=request("source-pcm","GET",frozenEndpoint+"/pcm?start=1000&count=1000&source_key="+frozen.at("source_key").get<std::string>()+"&timeline="+timeline,prepared::pcmContext(frozen,1000,1000));
        require(reopened.preparedPcm&&reopened.preparedPcm->bytes()==decoded.preparedPcm->bytes(),"Frozen save changed authoritative PCM");
        const std::string extension=source.at("sample_format")=="float64"?".w64":".wv";
        const auto destination=logPath.parent_path()/("native-export"+extension),rejected=logPath.parent_path()/("rejected"+extension);
        std::error_code ec;fs::remove(destination,ec);
        auto downloaded=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",prepared.at("variant").at("audio_sha256")}},0,destination});
        require(downloaded.error.empty(),"Artifact verification/download failed");
        downloaded=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",prepared.at("variant").at("audio_sha256")}},0,destination});
        require(downloaded.error.empty(),"Repeated verified export failed");
        auto corrupt=perform(connection,{"artifact","GET",prepared.at("download_url"),{{"expected_sha256",std::string(64,'0')}},0,rejected});
        require(!corrupt.error.empty() && !fs::exists(rejected) && !fs::exists(fs::path(rejected.wstring()+L".partial")),"Corrupt download and partial file must be removed");
        std::stop_source stopped;stopped.request_stop();require(!perform(connection,{"libraries","GET","/v1/libraries",nullptr,0,{}},stopped.get_token()).error.empty(),"Cancelled request should stop");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
        PcmBudget memory;NativeSession native({44100,2,2000,prepared::SampleType::Float32},"test","timeline",1,memory);
        const std::vector<float> pcm(2000,.1f);const auto bytes=std::as_bytes(std::span(pcm));
        auto block=std::make_shared<const prepared::Block>(prepared::NativeTimeline{44100,2,2000,prepared::SampleType::Float32},1000,1000,bytes);
        auto buffer=native.adopt(block,memory.reserve(bytes.size()),"test","timeline",1);
        Audition preview(memory);preview.start(buffer,{1000,2000},1000,true);SDL_Delay(100);
        require(!preview.snapshot().finished&&preview.snapshot().cursor>=1000&&preview.snapshot().cursor<2000,"Loop playhead must map into absolute source frames");
        preview.pause(true);const auto paused=preview.snapshot().cursor;SDL_Delay(80);require(preview.snapshot().cursor==paused,"Paused playhead must not advance");
        preview.loop(false);preview.pause(false);
        const auto until=SDL_GetTicks()+2000;while(!preview.snapshot().finished&&SDL_GetTicks()<until)SDL_Delay(10);
        require(preview.snapshot().finished&&preview.snapshot().cursor==2000,"Live loop disable must drain once and end at Out");preview.stop();
        const auto segment=[&](Frame origin){
            if(auto retained=native.find(origin))return retained;
            std::vector<float> values(2000,.1f);const auto data=std::as_bytes(std::span(values));
            auto item=std::make_shared<const prepared::Block>(prepared::NativeTimeline{44100,2,2000,prepared::SampleType::Float32},origin,1000,data);
            return native.adopt(item,memory.reserve(data.size()),"test","timeline",1);
        };
        preview.start(segment(0),{0,2000},0,false);preview.enqueue(segment(1000));
        const auto streamed=SDL_GetTicks()+2000;while(!preview.snapshot().finished&&SDL_GetTicks()<streamed)SDL_Delay(10);
        require(preview.snapshot().finished&&preview.snapshot().cursor==2000,"Prefetched PCM block handoff must preserve playback timeline");preview.stop();
        log<<"PASS native save/edit/replay, metadata search, collection, verified export and SDL loop lifetime\nALL PASS\n";
        return 0;
    }catch(const std::exception& ex){log<<"FAIL "<<ex.what()<<"\n";return 1;}
}
}

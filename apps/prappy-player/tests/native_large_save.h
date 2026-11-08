#pragma once
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static void largeSaveTest(const sampling::Connection& connection,const sampling::Json& source,const std::string& endpoint,const fs::path& directory){
    using namespace sampling;
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
    if(!SDL_Init(SDL_INIT_AUDIO))throw std::runtime_error(SDL_GetError());
    PcmBudget memory(64);NativePlayback player(connection,memory,[]{});
    const prepared::Frame total=source.at("frame_count");
    player.load(endpoint,source.at("capture").at("id"),source.at("capture").at("blob_sha256"),true,0,source);player.cache({0,total});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!player.state().loaded&&player.state().error.empty()&&std::chrono::steady_clock::now()<deadline){player.tick();Sleep(1);}
    if(!player.state().loaded)throw std::runtime_error("Large fixture editor did not open: "+player.state().error);
    auto clip=player.saveClip(0,total);player.stop(); // Snapshot must outlive its source/playback owner.
    if(clip->bytes()<=1024ULL*1024*1024||clip->bytes()<=memory.limit())throw std::runtime_error("Large save fixture does not exceed both required bounds.");
    const auto started=std::chrono::steady_clock::now();
    Request hash{"native-save-hash","LOCAL","",Json::object(),0,{}};hash.upload=clip;hash.incomingMemory=memory.reserve(2*1024*1024);
    auto fingerprint=perform(connection,hash);
    if(!fingerprint.error.empty())throw std::runtime_error(fingerprint.error);
    const auto hashed=std::chrono::steady_clock::now();
    const auto library=endpoint.substr(0,endpoint.find("/captures/"));
    Json body={{"request_id",uuid()},{"source_key",source.at("source_key")},{"operation","region.save"},{"pcm_sha256",fingerprint.body.at("sha256")},
        {"decoder_epoch",clip->decoderEpoch},{"decoder_epochs",clip->decoderEpochs},
        {"payload",{{"capture_id",source.at("capture").at("id")},{"timeline_id",source.at("timeline_id")},{"name","Bounded large save regression"},{"kind","texture"},{"span",{{"start_frame",0},{"end_frame",total}}}}}};
    Request upload{"command","POST",library+"/native-saves",body,0,{}};upload.upload=clip;
    std::stop_source cancel;std::jthread interrupt([&]{Sleep(100);cancel.request_stop();});
    const auto interrupted=perform(connection,upload,cancel.get_token());interrupt.join();
    if(interrupted.error.empty())throw std::runtime_error("Large in-flight upload ignored cancellation.");
    Request incomplete=upload;incomplete.upload.reset();const auto notPublished=perform(connection,incomplete);
    if(notPublished.status==200)throw std::runtime_error("Interrupted large upload published a partial region.");
    auto saved=perform(connection,upload);
    if(!saved.error.empty()||saved.status!=200)throw std::runtime_error("Large streamed upload failed: "+saved.error+" "+saved.body.dump());
    const auto uploaded=std::chrono::steady_clock::now();
    auto retry=upload;retry.upload.reset();const auto retried=perform(connection,retry);
    if(retried.status!=200||retried.body!=saved.body)throw std::runtime_error("Large native save retry was not idempotent.");
    const auto region=saved.body.at("region");const auto route=library+"/regions/"+region.at("id").get<std::string>()+"/revisions/"+std::to_string(region.at("revision").get<int>());
    auto frozen=perform(connection,Request{"source-descriptor","GET",route+"/audio",nullptr,0,{}});
    if(frozen.status!=200||frozen.body.at("manifest").at("raw_pcm_sha256")!=fingerprint.body.at("sha256")||
       frozen.body.at("coverage").at("start_frame")!=0||frozen.body.at("coverage").at("end_frame")!=total||
       frozen.body.at("sample_format")!=source.at("sample_format")||frozen.body.at("channels")!=source.at("channels"))
        throw std::runtime_error("Large frozen reopen changed native audio identity: "+frozen.body.dump());
    auto exported=perform(connection,Request{"export","POST",library+"/regions/"+region.at("id").get<std::string>()+"/exports/"+std::to_string(region.at("revision").get<int>()),Json::object(),0,{}});
    if(exported.status!=200||!exported.error.empty())throw std::runtime_error("Large export failed: "+exported.error+" "+exported.body.dump());
    Request download{"artifact","GET",exported.body.at("download_url"),{{"expected_sha256",exported.body.at("variant").at("audio_sha256")}},0,directory/"large-export.wv"};
    auto artifact=perform(connection,download);if(artifact.status!=200||!artifact.error.empty())throw std::runtime_error("Large export download failed: "+artifact.error);
    PROCESS_MEMORY_COUNTERS counters{};if(!GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))throw std::runtime_error("Cannot measure large save process memory.");
    Json result={{"raw_bytes",clip->bytes()},{"native_frames",total},{"sample_format",source.at("sample_format")},{"channels",source.at("channels")},
        {"raw_sha256",fingerprint.body.at("sha256")},{"hash_prepare_ms",std::chrono::duration<double,std::milli>(hashed-started).count()},
        {"upload_freeze_ms",std::chrono::duration<double,std::milli>(uploaded-hashed).count()},{"client_pcm_limit",memory.limit()},
        {"client_pcm_reserved_after_save",memory.used()},{"client_process_peak_rss",counters.PeakWorkingSetSize},{"region",region},
        {"export_path",utf8(download.download)},{"export",exported.body.at("variant")},{"reopen_verified",true},{"retry_idempotent",true},{"inflight_cancel_no_partial_publish",true},{"snapshot_outlived_source_owner",true}};
    stateio::write(directory/"large-save-result.json",result.dump(2));std::cout<<result.dump()<<'\n';
}

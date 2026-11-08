#include "sample_client.h"
#include "state_io.h"
#include "debug_log.h"
#include <SDL3/SDL_thread.h>
#include <winhttp.h>
#include <objbase.h>
#include <bcrypt.h>
#include <fstream>
#include <memory>
#include <array>
#include <cstring>
#include <cmath>
#include <atomic>
#include <algorithm>
namespace sampling {
namespace {
std::atomic<uint64_t> nextDiagnosticId{0};
struct Internet {
    HINTERNET value = nullptr;
    explicit Internet(HINTERNET h) : value(h) { if (!h) throw std::runtime_error("HTTP operation failed: "+std::to_string(GetLastError())); }
    ~Internet() { if(value) WinHttpCloseHandle(value); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
};
std::wstring wide(const std::string& s) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if (!count && !s.empty()) throw std::runtime_error("Invalid UTF-8 in server request.");
    std::wstring result(static_cast<size_t>(count),0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),result.data(),count);
    return result;
}
struct SourceFile {
    HANDLE handle=INVALID_HANDLE_VALUE;
    SourceFile(const fs::path& path,std::stop_token stop) {
        if(stop.stop_requested())throw std::runtime_error("File read cancelled.");
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open source file.");
    }
    ~SourceFile(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    DWORD read(void* buffer,DWORD count,std::stop_token stop) {
        if(stop.stop_requested())throw std::runtime_error("File read cancelled.");
        DWORD received=0;
        if(!ReadFile(handle,buffer,count,&received,nullptr))throw std::runtime_error("Source read failed or was cancelled.");
        return received;
    }
};
std::string readCredential(const fs::path& path,std::stop_token stop) {
    SourceFile file(path,stop);std::string token;std::array<char,4096> buffer{};
    while(const auto count=file.read(buffer.data(),static_cast<DWORD>(buffer.size()),stop)) {
        if(token.size()+count>65536)throw std::runtime_error("Token file exceeds the size limit.");
        token.append(buffer.data(),count);
    }
    return token;
}
void startWorkstationBackend() {
    debuglog::write("backend.start","distro=ubuntu26-dev");
    std::array<wchar_t,MAX_PATH> system{};
    const auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
    if(!length || length>=system.size())throw std::runtime_error("Cannot locate Windows WSL launcher.");
    const auto executable=fs::path(system.data())/L"wsl.exe";
    std::wstring command=L"\""+executable.wstring()+L"\" -d ubuntu26-dev -- bash /home/home/p/g/n/scraper/prappy.sh";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW|CREATE_NEW_PROCESS_GROUP,nullptr,nullptr,&startup,&process))
        throw std::runtime_error("Cannot start Workstation backend: Windows error "+std::to_string(GetLastError()));
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}

}
Connection authorize(Connection connection,std::stop_token stop) {
    if(connection.token.empty())connection.token=trim(readCredential(connection.credentialFile,stop));
    if(connection.token.size()<32)throw std::runtime_error("Token file is empty or invalid.");
    return connection;
}
std::string uuid() {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot generate command identity.");
    wchar_t buffer[40]{}; if(!StringFromGUID2(id,buffer,40))throw std::runtime_error("Cannot format command identity.");
    auto s=utf8(fs::path(buffer)); std::erase_if(s,[](char c){return c=='{' || c=='}' || c=='-';}); return s;
}
template<class Reader> static std::string sha256(Reader read,std::stop_token stop){
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("Cannot open SHA256.");
    const auto close=[](void* p){BCryptCloseAlgorithmProvider(p,0);};
    std::unique_ptr<void,decltype(close)> owner(algorithm,close);
    BCRYPT_HASH_HANDLE hash=nullptr;
    if (BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("Cannot create SHA256.");
    std::unique_ptr<void,decltype(&BCryptDestroyHash)> hashOwner(hash,BCryptDestroyHash);
    read([&](std::span<const std::byte> bytes){
        if(stop.stop_requested())throw std::runtime_error("SHA256 cancelled.");
        if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),static_cast<ULONG>(bytes.size()),0)<0)throw std::runtime_error("SHA256 failed.");
    });
    std::array<unsigned char,32> digest{};
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("SHA256 failed.");
    const char* digits="0123456789abcdef"; std::string out;
    for (auto c:digest) {out+=digits[c>>4];out+=digits[c&15];}
    return out;
}
static std::string fileHash(const fs::path& path,std::stop_token stop){
    const auto began=debuglog::Clock::now();debuglog::write("artifact.verify.begin","file="+utf8(path.filename()));
    SourceFile file(path,stop);std::vector<std::byte> buffer(65536);
    auto result=sha256([&](auto consume){while(const auto count=file.read(buffer.data(),static_cast<DWORD>(buffer.size()),stop))consume(std::span<const std::byte>(buffer.data(),count));},stop);
    debuglog::write("artifact.verify.end","ms="+std::to_string(debuglog::elapsed(began)));return result;
}

Reply perform(const Connection& connection,const Request& request,std::stop_token stop) {
    Reply reply; reply.request=request;
    if(request.tag=="native-save-hash"){
        const auto started=debuglog::Clock::now();
        try{
            if(!request.upload)throw std::runtime_error("Save has no retained PCM snapshot.");
            const auto digest=sha256([&](auto consume){request.upload->each(consume,stop);},stop);
            reply.status=200;reply.body={{"sha256",digest},{"bytes",request.upload->bytes()},{"decoder_epoch",request.upload->decoderEpoch},{"decoder_epochs",request.upload->decoderEpochs}};
        }catch(const std::exception& e){reply.error=e.what();}
        debuglog::write("save.hash","ms="+std::to_string(debuglog::elapsed(started))+" ok="+(reply.error.empty()?"1":"0"));return reply;
    }
    const auto began=debuglog::Clock::now();
    const auto identity="req="+std::to_string(request.diagnosticId?request.diagnosticId:++nextDiagnosticId)+" tag="+request.tag+" gen="+std::to_string(request.generation);
    debuglog::write("http.begin",identity);
    struct LogCompletion {
        const Reply& reply;const std::string& identity;debuglog::Clock::time_point began;
        ~LogCompletion(){debuglog::write("http.end",identity+" ms="+std::to_string(debuglog::elapsed(began))+" status="+std::to_string(reply.status)+" ok="+(reply.error.empty()?"1":"0"));}
    } completion{reply,identity,began};
    fs::path temporary;
    try {
        auto base=wide(connection.url); URL_COMPONENTS url{};url.dwStructSize=sizeof(url);
        url.dwHostNameLength=static_cast<DWORD>(-1);url.dwUrlPathLength=static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(base.c_str(),0,0,&url)) throw std::runtime_error("Invalid backend URL.");
        if(!url.lpszHostName || !url.dwHostNameLength)throw std::runtime_error("Backend URL requires a host.");
        const std::wstring host(url.lpszHostName,url.dwHostNameLength);
        if (url.nScheme!=INTERNET_SCHEME_HTTPS && host!=L"127.0.0.1" && host!=L"localhost" && host!=L"::1")
            throw std::runtime_error("Remote servers require HTTPS.");
        Internet session(WinHttpOpen(L"Prappy Sampling/1",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0));
        // Explicit exports may render audio; interactive reads stay bounded.
        const int receiveTimeout=request.tag=="export"?600000:30000;
        if(!WinHttpSetTimeouts(session.value,5000,5000,30000,receiveTimeout))
            throw std::runtime_error("Cannot configure backend timeouts.");
        Internet server(WinHttpConnect(session.value,host.c_str(),url.nPort,0));
        const auto method=wide(request.method),path=wide(request.path);
        Internet operation(WinHttpOpenRequest(server.value,method.c_str(),path.c_str(),nullptr,WINHTTP_NO_REFERER,
                 WINHTTP_DEFAULT_ACCEPT_TYPES,url.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        // Closing an in-flight WinHTTP request interrupts reads and connection waits.
        // The callback is destroyed before the owning handle, and releases ownership once.
        std::atomic<HINTERNET> active{operation.value};
        struct Release { Internet& owner;std::atomic<HINTERNET>& handle;~Release(){owner.value=handle.exchange(nullptr);} } release{operation,active};
        const auto cancel=[&]{if(auto handle=active.exchange(nullptr))WinHttpCloseHandle(handle);};
        std::stop_callback cancellation(stop,cancel);

        if(stop.stop_requested())throw std::runtime_error("Request cancelled.");
        const auto token=authorize(connection,stop).token;
        DWORD redirectPolicy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if(!WinHttpSetOption(operation.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirectPolicy,sizeof(redirectPolicy)))throw std::runtime_error("Cannot constrain backend redirects.");
        auto headers=wide("Authorization: Bearer "+token+"\r\nContent-Type: "+std::string(request.upload?"application/octet-stream":"application/json")+"\r\nX-Prappy-Request-Id: "+std::to_string(GetCurrentProcessId())+"_"+std::to_string(request.diagnosticId)+"\r\n");
        const auto body=request.method=="GET" || request.body.is_null() ? std::string{} : request.body.dump();
        bool sent=false;
        if(request.upload){
            const UploadFrame frame(body.size(),request.upload->bytes());const auto length=frame.metadata;
            headers+=L"Content-Length: "+std::to_wstring(frame.total)+L"\r\n";
            sent=WinHttpSendRequest(operation.value,headers.c_str(),static_cast<DWORD>(headers.size()),WINHTTP_NO_REQUEST_DATA,0,frame.sendLength,0)!=FALSE;
            const auto write=[&](std::span<const std::byte> bytes){
                size_t at=0;
                while(at<bytes.size()){
                    if(stop.stop_requested())throw std::runtime_error("Native save upload cancelled.");
                    DWORD written=0;const auto count=static_cast<DWORD>(std::min<size_t>(65536,bytes.size()-at));
                    if(!WinHttpWriteData(operation.value,bytes.data()+at,count,&written)||!written)throw std::runtime_error("Native save upload failed: "+std::to_string(GetLastError()));
                    at+=written;
                }
            };
            if(sent){write(std::as_bytes(std::span(&length,1)));write(std::as_bytes(std::span(body.data(),body.size())));request.upload->each(write,stop);}
        }else sent=WinHttpSendRequest(operation.value,headers.c_str(),static_cast<DWORD>(headers.size()),
            body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0)!=FALSE;
        if(!sent||!WinHttpReceiveResponse(operation.value,nullptr)){
            const auto error=GetLastError();debuglog::write("http.failure",identity+" win32="+std::to_string(error));
            throw std::runtime_error("Backend connection failed: "+std::to_string(error));
        }
        DWORD status=0,length=sizeof(status);
        WinHttpQueryHeaders(operation.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr);
        debuglog::write("http.headers",identity+" ms="+std::to_string(debuglog::elapsed(began))+" status="+std::to_string(status));
        reply.status=static_cast<int>(status);
        std::array<wchar_t,256> serverTiming{};DWORD timingSize=sizeof(serverTiming);
        if(WinHttpQueryHeaders(operation.value,WINHTTP_QUERY_CUSTOM,L"Server-Timing",serverTiming.data(),&timingSize,nullptr))
            debuglog::write("http.server",identity+" "+utf8(fs::path(serverTiming.data())));
        std::ofstream output;
        if (!request.download.empty() && status==200) {
            fs::create_directories(request.download.parent_path());
            temporary=request.download.wstring()+L".partial"; output.open(temporary,std::ios::binary|std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot create downloaded audio.");
        }
        std::vector<char> bytes,chunk(65536);size_t total=0;
        size_t capacity=request.path.ends_with("/native-saves")?1024*1024:64ULL*1024*1024;
        if(status==200&&(request.tag=="source-pcm"||request.tag=="source-waveform")){
            const auto channels=request.body.at("channels").get<int64_t>();
            const auto count=request.body.at(request.tag=="source-pcm"?"frame_count":"bin_count").get<int64_t>();
            const auto stride=request.tag=="source-pcm"?prepared::sampleBytes(prepared::sampleType(request.body.at("sample_format").get<std::string>())):8;
            if(channels<1||channels>32||count<1||static_cast<uint64_t>(count)>32ULL*1024*1024/(channels*stride))
                throw std::runtime_error("Invalid or oversized native response context.");
            capacity=static_cast<size_t>(count)*static_cast<size_t>(channels)*stride;bytes.reserve(capacity);
        }
        for (;;) {
            DWORD count=0;
            if (!WinHttpReadData(operation.value,chunk.data(),static_cast<DWORD>(chunk.size()),&count)) throw std::runtime_error("Backend response interrupted.");
            if (!count) break;
            total+=count;
            if (total>(output.is_open()?2ULL*1024*1024*1024:capacity)) throw std::runtime_error("Backend response exceeds resource limit.");
            if (output.is_open()) {output.write(chunk.data(),count);if(!output) throw std::runtime_error("Cannot write downloaded audio.");}
            else bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+count);
        }
        if (status<200 || status>=300) {
            const auto error=Json::parse(bytes.begin(),bytes.end(),nullptr,false);
            throw std::runtime_error("HTTP "+std::to_string(status)+": "+(error.is_discarded()?std::string(bytes.begin(),bytes.end()):error.dump()));
        }
        if (output.is_open()) {
            output.close();
            if(request.body.is_object() && request.body.contains("expected_sha256") && fileHash(temporary,stop)!=request.body.at("expected_sha256").get<std::string>())throw std::runtime_error("Downloaded artifact hash differs.");
            if(!MoveFileExW(temporary.c_str(),request.download.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish downloaded audio.");reply.body={{"path",utf8(request.download)}};
        }
        else if (status==202){reply.body=Json::parse(bytes.begin(),bytes.end());}
        else if (request.tag=="source-pcm" || request.tag=="source-waveform") {
            const auto header=[&](const wchar_t* name){
                std::array<wchar_t,256> value{};DWORD size=static_cast<DWORD>(sizeof(value));
                if(!WinHttpQueryHeaders(operation.value,WINHTTP_QUERY_CUSTOM,name,value.data(),&size,nullptr))throw std::runtime_error("Missing prepared audio identity.");
                return utf8(fs::path(value.data()));
            };
            // Hash through the same Windows provider as downloaded artifacts, without disk I/O.
            BCRYPT_ALG_HANDLE algorithm=nullptr;
            if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot open PCM SHA256.");
            std::array<unsigned char,32> digest{};
            const auto hashStatus=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size()));
            BCryptCloseAlgorithmProvider(algorithm,0);
            if(hashStatus<0)throw std::runtime_error("PCM SHA256 failed.");
            std::string hash;const char* digits="0123456789abcdef";for(auto c:digest){hash+=digits[c>>4];hash+=digits[c&15];}
            if(hash!=header(L"X-PCM-SHA256"))throw std::runtime_error("Prepared audio hash differs.");
            {
                const auto& expected=request.body;
                if(header(L"X-Source-Key")!=expected.at("source_key").get<std::string>()||
                   header(L"X-Channels")!=std::to_string(expected.at("channels").get<int64_t>())||
                   header(L"X-Timeline-Id")!=expected.at("timeline_id").get<std::string>())
                    throw std::runtime_error("Prepared response source or channel identity differs.");
                if(request.tag=="source-pcm"&&header(L"X-Audio-Domain")!=expected.at("domain").get<std::string>())
                    throw std::runtime_error("Prepared response audio domain differs.");
                reply.body=expected;
                if(request.tag=="source-pcm")reply.body["decoder_epoch"]=header(L"X-Decoder-Epoch");
                if(request.tag=="source-pcm") {
                    const auto start=expected.at("origin_frame").get<int64_t>(),count=expected.at("frame_count").get<int64_t>();
                    if(header(L"X-Origin-Frame")!=std::to_string(start)||header(L"X-Frame-Count")!=std::to_string(count)||
                       header(L"X-Sample-Rate")!=std::to_string(expected.at("sample_rate").get<int64_t>())||
                       header(L"X-Sample-Format")!=expected.at("sample_format").get<std::string>())
                        throw std::runtime_error("Prepared PCM timeline or sample type differs.");
                    prepared::NativeTimeline source{expected.at("sample_rate"),expected.at("channels"),
                        expected.at("total_frames"),prepared::sampleType(expected.at("sample_format").get<std::string>())};
                    reply.preparedPcm=std::make_shared<prepared::Block>(source,start,count,std::span(
                        reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
                } else {
                    const auto count=expected.at("bin_count").get<int64_t>(),channels=expected.at("channels").get<int64_t>();
                    if(count<=0||count>8192||channels<=0||channels>32||
                       header(L"X-Bin-Count")!=std::to_string(count)||
                       header(L"X-Start-Bin")!=std::to_string(expected.at("start_bin").get<int64_t>())||
                       header(L"X-Frames-Per-Bin")!=std::to_string(expected.at("frames_per_bin").get<int64_t>())||
                       bytes.size()!=static_cast<size_t>(count*channels*8))throw std::runtime_error("Prepared waveform bounds differ.");
                    reply.pcm.resize(bytes.size()/4);std::memcpy(reply.pcm.data(),bytes.data(),bytes.size());
                    for(auto value:reply.pcm)if(!std::isfinite(value))throw std::runtime_error("Nonfinite waveform.");
                }
            }
        } else {
            reply.body=Json::parse(bytes.begin(),bytes.end());
        }
    } catch(const std::exception& e) {reply.error=e.what();if(!temporary.empty()){std::error_code ec;fs::remove(temporary,ec);}}
    reply.request.incomingMemory.reset();
    return reply;
}

struct ClientState {
    Connection connection;
    Client::Transport transport;
    Client::CredentialReader reader;
    Client::Bootstrap bootstrap;
    std::stop_source stop;
    // The executor mutex protects these fields, except notification, which has
    // its own gate so retirement also fences callbacks already being delivered.
    std::deque<Request> pending;
    std::vector<Reply> completed;
    bool retired=false,credentialsStarted=false,credentialsReady=false;
    std::string credentialError;
    std::mutex notificationMutex;
    std::function<void()> notify;
};
namespace {
size_t requestLane(const Request& request) {
    if(request.tag=="source-pcm" || request.tag=="source-waveform")return 0;
    if(request.tag=="libraries"||request.method!="GET")return 1;
    return 2;
}
// One process-wide executor, with exactly three independently scheduled lanes.
// No Client owns a thread or joins on retirement. Weak registrations and the
// three active slots bound retired state even when a redirector ignores cancel.
// Workers join at process teardown, after the UI and instance lock are gone.
class ClientExecutor {
    struct Lane {size_t index=0;std::jthread worker;std::shared_ptr<ClientState> active;std::optional<Request> request;std::stop_source cancel;};
    std::array<Lane,3> lanes;
    std::vector<std::weak_ptr<ClientState>> clients;
    std::deque<std::shared_ptr<ClientState>> cleanup;
    static bool closesSession(const Request& request){return request.tag=="source-session-close"&&request.method=="DELETE";}

    std::shared_ptr<ClientState> find(size_t index) {
        std::erase_if(cleanup,[&](const auto& state){return state->pending.empty()&&std::none_of(lanes.begin(),lanes.end(),[&](const auto& lane){return lane.active==state;});});
        if(index==1)for(const auto& state:cleanup)if(!state->pending.empty())return state;

        for(auto& weak:clients)if(auto state=weak.lock();state&&!state->retired) {
            if(index==1&&!state->credentialsStarted)return state;
            if(std::any_of(state->pending.begin(),state->pending.end(),[&](const Request& request){return requestLane(request)==index&&state->credentialsReady;}))return state;
        }
        return {};
    }
    void run(Lane& lane,std::stop_token shutdown) {
        while(!shutdown.stop_requested()) {
            std::shared_ptr<ClientState> state;Request request;Connection connection;bool credentials=false;
            {
                std::unique_lock guard(mutex);
                if(!wake.wait(guard,shutdown,[&]{return static_cast<bool>(find(lane.index));}))return;
                if(shutdown.stop_requested())return;
                state=find(lane.index);lane.active=state;
                credentials=lane.index==1&&!state->credentialsStarted;
                if(credentials)state->credentialsStarted=true;
                else {
                    const auto next=std::find_if(state->pending.begin(),state->pending.end(),[&](const Request& item){return requestLane(item)==lane.index&&state->credentialsReady;});
                    request=std::move(*next);state->pending.erase(next);
                    lane.cancel=std::stop_source{};lane.request=request;
                }
                connection=state->connection;
            }
            const auto stop=credentials?state->stop.get_token():lane.cancel.get_token();
            auto cancellation=lane.cancel;
            std::stop_callback retired(state->stop.get_token(),[cancellation]() mutable{cancellation.request_stop();});
            if(credentials) {
                const auto credentialsStart=debuglog::Clock::now();
                std::string error;
                const auto read=[&] {
                    if(stop.stop_requested())throw std::runtime_error("Credential preparation cancelled.");
                    if(connection.token.empty())connection.token=trim(state->reader(connection.credentialFile,stop));
                    if(connection.token.size()<32)throw std::runtime_error("Token file is empty or invalid.");
                };
                try {read();}catch(const std::exception& e){error=e.what();connection.token.clear();}
                if(!error.empty()&&connection.startWorkstation&&!stop.stop_requested()) {
                    try {
                        state->bootstrap();
                        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
                        do {
                            if(stop.stop_requested())break;
                            try {read();error.clear();}catch(const std::exception& e){error=e.what();connection.token.clear();}
                            if(error.empty())break;
                            {std::unique_lock guard(mutex);wake.wait_until(guard,stop,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(250)),[]{return false;});}
                        }while(!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline);
                    }catch(const std::exception& e){error=e.what();}
                }
                debuglog::write("backend.credentials","ms="+std::to_string(debuglog::elapsed(credentialsStart))+" ok="+(error.empty()?"1":"0"));
                std::lock_guard guard(mutex);
                if(!state->retired){state->connection=std::move(connection);state->credentialError=std::move(error);state->credentialsReady=true;}
                lane.active.reset();wake.notify_all();
            } else {
                debuglog::write("http.dequeue","req="+std::to_string(request.diagnosticId)+" tag="+request.tag+" queue_ms="+std::to_string(debuglog::elapsed(request.queuedAt)));
                Reply result;result.request=request;
                try {
                    if(!state->credentialError.empty())result.error=state->credentialError;
                    else if(!stop.stop_requested())result=state->transport(connection,request,stop);
                    if((request.tag=="libraries"||request.tag=="catalog")&&!result.error.empty()&&state->credentialError.empty()&&connection.startWorkstation&&!stop.stop_requested()) {
                        state->bootstrap();
                        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
                        do {
                            {std::unique_lock guard(mutex);wake.wait_for(guard,stop,std::chrono::milliseconds(250),[]{return false;});}
                            if(stop.stop_requested())break;
                            result=state->transport(connection,request,stop);
                        }while(!result.error.empty()&&std::chrono::steady_clock::now()<deadline);
                        if(!result.error.empty()&&!stop.stop_requested())result.error="Workstation backend did not start. See ~/.local/state/audioharvest/prappy-server.log in WSL. "+result.error;
                    }
                }catch(const std::exception& e){result.error=e.what();}
                // Transport implementations cannot change a request's identity.
                result.request=request;result.request.incomingMemory.reset();request.incomingMemory.reset();
                bool deliver=false;
                {std::lock_guard guard(mutex);if(!state->retired&&!stop.stop_requested()){state->completed.push_back(std::move(result));deliver=true;}lane.active.reset();lane.request.reset();}
                if(deliver){std::lock_guard guard(state->notificationMutex);if(state->notify)state->notify();}
            }
        }
    }
    ClientExecutor() {
        try {
            for(size_t index=0;index<lanes.size();++index){lanes[index].index=index;lanes[index].worker=std::jthread([this,index](std::stop_token stop){run(lanes[index],stop);SDL_CleanupTLS();});}
        }catch(...) {
            for(auto& lane:lanes)lane.worker.request_stop();
            wake.notify_all();
            for(auto& lane:lanes)if(lane.worker.joinable())lane.worker.join();
            throw;
        }
    }
public:
    std::mutex mutex;
    std::condition_variable_any wake;
    ~ClientExecutor() {
        std::array<std::shared_ptr<ClientState>,3> active;
        for(auto& lane:lanes)lane.worker.request_stop();
        {
            std::lock_guard guard(mutex);
            for(size_t index=0;index<lanes.size();++index){active[index]=lanes[index].active;if(active[index])CancelSynchronousIo(lanes[index].worker.native_handle());}
        }
        for(auto& state:active)if(state)state->stop.request_stop();
        wake.notify_all();
        // Join before destroying synchronization primitives used by workers.
        for(auto& lane:lanes)if(lane.worker.joinable())lane.worker.join();
    }
    static ClientExecutor& instance(){static ClientExecutor executor;return executor;}
    void add(const std::shared_ptr<ClientState>& state) {
        std::lock_guard guard(mutex);
        std::erase_if(clients,[](const auto& weak){auto value=weak.lock();return !value||value->retired;});
        clients.push_back(state);wake.notify_all();
    }
    static bool transient(const Request& request){
        return request.method=="GET"&&request.download.empty()&&request.tag!="libraries"&&request.tag!="capabilities"&&request.tag!="catalog"&&request.tag!="collections";
    }
    static bool sameRead(const Request& old,const Request& request){
        if(old.tag!=request.tag)return false;
        const auto flag=[](const Request& item,const char* key){return item.body.is_object()&&item.body.value(key,false);};
        if(request.tag=="source-pcm")return flag(old,"audition")==flag(request,"audition");
        if(request.tag=="source-waveform")return flag(old,"overview")==flag(request,"overview");
        return request.tag!="collectionMember"||old.path==request.path;
    }
    void submit(const std::shared_ptr<ClientState>& state,Request request){
        std::vector<std::stop_source> stops;
        {
            std::lock_guard guard(mutex);
            if(request.method=="GET"&&request.download.empty()){
                const auto superseded=[&](const Request& old){
                    return old.method=="GET"&&old.download.empty()&&((transient(old)&&old.generation<request.generation)||sameRead(old,request));
                };
                std::erase_if(state->pending,superseded);
                for(auto& lane:lanes)if(lane.active==state&&lane.request&&superseded(*lane.request))stops.push_back(lane.cancel);
            }
            if(state->pending.size()>=512)throw std::runtime_error("Backend request queue is full.");
            state->pending.push_back(std::move(request));wake.notify_all();
        }
        for(auto& stop:stops)stop.request_stop();
    }
    void cancelReads(const std::shared_ptr<ClientState>& state,ReadScope scope){
        std::vector<std::stop_source> stops;
        const auto selected=[&](const Request& request){
            if(request.tag=="source-waveform")return scope==ReadScope::Audio;
            return request.tag=="source-pcm"&&(scope!=ReadScope::Audition||(request.body.is_object()&&request.body.value("audition",false)));
        };
        {
            std::lock_guard guard(mutex);std::erase_if(state->pending,selected);
            for(auto& lane:lanes)if(lane.active==state&&lane.request&&selected(*lane.request))stops.push_back(lane.cancel);
        }
        for(auto& stop:stops)stop.request_stop();
    }
    void retire(const std::shared_ptr<ClientState>& state) {
        {std::lock_guard guard(state->notificationMutex);state->notify={};}
        {
            std::lock_guard guard(mutex);
            // Only idempotent lease release survives retirement; user commands and reads do not.
            auto closing=std::make_shared<ClientState>();closing->connection=state->connection;closing->transport=state->transport;
            closing->credentialsReady=closing->credentialsStarted=true;
            for(auto& request:state->pending)if(closesSession(request))closing->pending.push_back(std::move(request));
            for(const auto& lane:lanes)if(lane.active==state&&lane.request&&closesSession(*lane.request))closing->pending.push_back(*lane.request);
            if(!closing->pending.empty()&&state->credentialsReady&&state->credentialError.empty()){
                if(cleanup.size()<512)cleanup.push_back(std::move(closing));
                else debuglog::write("backend.session_close","cleanup queue full; server lease expiry will release session");
            }
            state->retired=true;state->pending.clear();state->completed.clear();
            for(auto& lane:lanes)if(lane.active==state)CancelSynchronousIo(lane.worker.native_handle());
            std::erase_if(clients,[&](const auto& weak){auto value=weak.lock();return !value||value==state;});
        }
        state->stop.request_stop();wake.notify_all();
    }
};
}
Client::Client(Connection connection,std::function<void()> notify,Transport transport,CredentialReader reader,Bootstrap bootstrap):state(std::make_shared<ClientState>()) {
    (void)debuglog::instance(); // Construct before the executor's worker threads.
    state->connection=std::move(connection);state->notify=std::move(notify);
    if(!state->connection.token.empty()) {
        state->credentialsStarted=true;state->credentialsReady=true;
        if(state->connection.token.size()<32)state->credentialError="Token file is empty or invalid.";
    }
    state->transport=transport?std::move(transport):perform;
    state->reader=reader?std::move(reader):readCredential;
    state->bootstrap=bootstrap?std::move(bootstrap):startWorkstationBackend;
    ClientExecutor::instance().add(state);
}
Client::~Client(){ClientExecutor::instance().retire(state);}
void Client::submit(Request request) {
    request.queuedAt=debuglog::Clock::now();request.diagnosticId=++nextDiagnosticId;
    ClientExecutor::instance().submit(state,std::move(request));
}
void Client::cancelReads(ReadScope scope){ClientExecutor::instance().cancelReads(state,scope);}
std::vector<Reply> Client::collect(){std::lock_guard guard(ClientExecutor::instance().mutex);auto result=std::move(state->completed);state->completed.clear();return result;}
}

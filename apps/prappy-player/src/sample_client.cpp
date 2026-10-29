#include "sample_client.h"
#include "state_io.h"
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
void startWorkstationBackend() {
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
std::string uuid() {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot generate command identity.");
    wchar_t buffer[40]{}; if(!StringFromGUID2(id,buffer,40))throw std::runtime_error("Cannot format command identity.");
    auto s=utf8(fs::path(buffer)); std::erase_if(s,[](char c){return c=='{' || c=='}' || c=='-';}); return s;
}
std::string fileHash(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("Cannot open SHA256.");
    const auto close=[](void* p){BCryptCloseAlgorithmProvider(p,0);};
    std::unique_ptr<void,decltype(close)> owner(algorithm,close);
    BCRYPT_HASH_HANDLE hash=nullptr;
    if (BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("Cannot create SHA256.");
    std::unique_ptr<void,decltype(&BCryptDestroyHash)> hashOwner(hash,BCryptDestroyHash);
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("Cannot read source for identity.");
    std::vector<char> buffer(65536);
    while (file.read(buffer.data(),buffer.size()) || file.gcount()) {
        if (BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),static_cast<ULONG>(file.gcount()),0)<0) throw std::runtime_error("SHA256 failed.");
    }
    if (!file.eof()) throw std::runtime_error("Source read failed.");
    std::array<unsigned char,32> digest{};
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("SHA256 failed.");
    const char* digits="0123456789abcdef"; std::string out;
    for (auto c:digest) {out+=digits[c>>4];out+=digits[c&15];} return out;
}
Reply perform(const Connection& connection,const Request& request,std::stop_token stop) {
    Reply reply; reply.request=request;
    fs::path temporary;
    try {
        if (request.tag=="hash") { reply.body={{"sha256",fileHash(fs::path(std::u8string(reinterpret_cast<const char8_t*>(request.path.data()),request.path.size())))}}; return reply; }
        auto base=wide(connection.url); URL_COMPONENTS url{};url.dwStructSize=sizeof(url);
        url.dwHostNameLength=static_cast<DWORD>(-1);url.dwUrlPathLength=static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(base.c_str(),0,0,&url)) throw std::runtime_error("Invalid backend URL.");
        if(!url.lpszHostName || !url.dwHostNameLength)throw std::runtime_error("Backend URL requires a host.");
        const std::wstring host(url.lpszHostName,url.dwHostNameLength);
        if (url.nScheme!=INTERNET_SCHEME_HTTPS && host!=L"127.0.0.1" && host!=L"localhost" && host!=L"::1")
            throw std::runtime_error("Remote servers require HTTPS.");
        Internet session(WinHttpOpen(L"Prappy Sampling/1",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0));
        WinHttpSetTimeouts(session.value,5000,5000,30000,30000);
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
        const auto token=connection.token.empty()?trim(stateio::read(connection.credentialFile)):connection.token;
        if(token.size()<32)throw std::runtime_error("Token file is empty or invalid.");
        DWORD redirectPolicy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if(!WinHttpSetOption(operation.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirectPolicy,sizeof(redirectPolicy)))throw std::runtime_error("Cannot constrain backend redirects.");
        auto headers=wide("Authorization: Bearer "+token+"\r\nContent-Type: application/json\r\n");
        const auto body=request.method=="GET" || request.body.is_null() ? std::string{} : request.body.dump();
        if (!WinHttpSendRequest(operation.value,headers.c_str(),static_cast<DWORD>(headers.size()),
             body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0) ||
            !WinHttpReceiveResponse(operation.value,nullptr)) throw std::runtime_error("Backend connection failed: "+std::to_string(GetLastError()));
        DWORD status=0,length=sizeof(status);
        WinHttpQueryHeaders(operation.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr);
        reply.status=static_cast<int>(status);
        std::ofstream output;
        if (!request.download.empty() && status==200) {
            fs::create_directories(request.download.parent_path());
            temporary=request.download.wstring()+L".partial"; output.open(temporary,std::ios::binary|std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot create downloaded audio.");
        }
        std::vector<char> bytes,chunk(65536);size_t total=0;
        for (;;) {
            DWORD count=0;
            if (!WinHttpReadData(operation.value,chunk.data(),static_cast<DWORD>(chunk.size()),&count)) throw std::runtime_error("Backend response interrupted.");
            if (!count) break;
            total+=count;
            if (total>(output.is_open()?2ULL*1024*1024*1024:((request.tag=="pcm" || request.tag=="audition")?48000ULL*600*8:64ULL*1024*1024))) throw std::runtime_error("Backend response exceeds resource limit.");
            if (output.is_open()) {output.write(chunk.data(),count);if(!output) throw std::runtime_error("Cannot write downloaded audio.");}
            else bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+count);
        }
        if (status<200 || status>=300) {
            const auto error=Json::parse(bytes.begin(),bytes.end(),nullptr,false);
            throw std::runtime_error("HTTP "+std::to_string(status)+": "+(error.is_discarded()?std::string(bytes.begin(),bytes.end()):error.dump()));
        }
        if (output.is_open()) {
            output.close();
            if(request.body.is_object() && request.body.contains("expected_sha256") && fileHash(temporary)!=request.body.at("expected_sha256").get<std::string>())throw std::runtime_error("Downloaded artifact hash differs.");
            if(!MoveFileExW(temporary.c_str(),request.download.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish downloaded audio.");reply.body={{"path",utf8(request.download)}};
        }
        else if (request.tag=="pcm" || request.tag=="audition") {
            const auto header=[&](const wchar_t* name){
                std::array<wchar_t,256> value{};DWORD size=static_cast<DWORD>(sizeof(value));
                if(!WinHttpQueryHeaders(operation.value,WINHTTP_QUERY_CUSTOM,name,value.data(),&size,nullptr))throw std::runtime_error("Missing canonical PCM identity.");
                return utf8(fs::path(value.data()));
            };
            const auto at=request.path.find("start="),until=request.path.find("&end="),timeline=request.path.find("&timeline=");
            if(at==std::string::npos||until==std::string::npos||timeline==std::string::npos)throw std::runtime_error("PCM request lacks exact bounds.");
            const auto start=std::stoll(request.path.substr(at+6,until-at-6)),end=std::stoll(request.path.substr(until+5,timeline-until-5));
            if(header(L"X-Origin-Frame")!=std::to_string(start)||header(L"X-Frame-Count")!=std::to_string(end-start)||header(L"X-Timeline-Id")!=request.path.substr(timeline+10)||bytes.size()!=static_cast<size_t>(end-start)*8)throw std::runtime_error("Canonical PCM timeline or bounds differ.");
            // Hash through the same Windows provider as downloaded artifacts, without disk I/O.
            BCRYPT_ALG_HANDLE algorithm=nullptr;
            if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot open PCM SHA256.");
            std::array<unsigned char,32> digest{};
            const auto hashStatus=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size()));
            BCryptCloseAlgorithmProvider(algorithm,0);
            if(hashStatus<0)throw std::runtime_error("PCM SHA256 failed.");
            std::string hash;const char* digits="0123456789abcdef";for(auto c:digest){hash+=digits[c>>4];hash+=digits[c&15];}
            if(hash!=header(L"X-PCM-SHA256"))throw std::runtime_error("Canonical PCM hash differs.");
            reply.body={{"origin_frame",start},{"frame_count",end-start},{"decode_identity",header(L"X-Decode-Identity")}};
            if (bytes.empty() || bytes.size()%8) throw std::runtime_error("Invalid stereo PCM response.");
            reply.pcm.resize(bytes.size()/4);std::memcpy(reply.pcm.data(),bytes.data(),bytes.size());
            for(auto value:reply.pcm) if(!std::isfinite(value)) throw std::runtime_error("Nonfinite PCM.");
        } else {
            reply.body=Json::parse(bytes.begin(),bytes.end());
        }
    } catch(const std::exception& e) {reply.error=e.what();if(!temporary.empty()){std::error_code ec;fs::remove(temporary,ec);}}
    return reply;
}
size_t Client::lane(const Request& request) {
    if(request.tag=="audition")return 0;
    if(request.tag=="libraries"||request.tag=="hash"||request.tag=="resolve"||request.method!="GET")return 1;
    return 2;
}
Client::Client(Connection c,std::function<void()> n) : connection(std::move(c)),notify(std::move(n)) {
    for(size_t i=0;i<workers.size();++i)workers[i]=std::jthread([this,i](std::stop_token stop){run(stop,i);});
}
void Client::run(std::stop_token stop,size_t index){
    while(!stop.stop_requested()) {
        Request request;
        {std::unique_lock lock(mutex);
            const auto find=[&]{return std::find_if(pending.begin(),pending.end(),[&](const auto& item){return lane(item)==index;});};
            if(!wake.wait(lock,stop,[&]{return find()!=pending.end();}))break;
            const auto next=find();request=std::move(*next);pending.erase(next);
        }
        auto result=perform(connection,request,stop);
        if(request.tag=="libraries" && !result.error.empty() && connection.startWorkstation && !stop.stop_requested()) {
            try {
                startWorkstationBackend();
                // Retry the same endpoint while WSL starts, without touching the UI thread.
                // The launcher lock prevents duplicate servers across player instances.
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
                do {
                    {std::unique_lock lock(mutex);wake.wait_for(lock,stop,std::chrono::milliseconds(250),[]{return false;});}
                    if(stop.stop_requested())break;
                    result=perform(connection,request,stop);
                } while(!result.error.empty() && std::chrono::steady_clock::now()<deadline);
                if(!result.error.empty() && !stop.stop_requested())result.error="Workstation backend did not start. See ~/.local/state/audioharvest/prappy-server.log in WSL. "+result.error;
            }catch(const std::exception& e){result.error=e.what();}
        }
        {std::lock_guard lock(mutex);completed.push_back(std::move(result));}
        if(notify)notify();
    }
 }
Client::~Client(){for(auto& worker:workers)worker.request_stop();wake.notify_all();}
void Client::submit(Request r) {
    std::lock_guard lock(mutex);
    if(r.method=="GET" && r.tag!="download") std::erase_if(pending,[&](const auto& old){return old.tag==r.tag && (r.tag!="collectionMember" || old.path==r.path);});
    if(pending.size()>=512) throw std::runtime_error("Backend request queue is full.");
    pending.push_back(std::move(r));wake.notify_all();
}
std::vector<Reply> Client::collect(){std::lock_guard lock(mutex);auto result=std::move(completed);completed.clear();return result;}
}

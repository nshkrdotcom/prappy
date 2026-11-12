#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dbgeng.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <wrl/client.h>
#include <cstring>
#include <json.hpp>
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
constexpr size_t MaxEnvelope=64*1024*1024;
struct StorageError:std::runtime_error{using std::runtime_error::runtime_error;};
struct UniqueHandle {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit UniqueHandle(HANDLE value=INVALID_HANDLE_VALUE)noexcept:handle(value){}
    ~UniqueHandle(){if(handle&&handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    UniqueHandle(const UniqueHandle&)=delete;UniqueHandle& operator=(const UniqueHandle&)=delete;
    HANDLE get()const noexcept{return handle;}
    bool valid()const noexcept{return handle&&handle!=INVALID_HANDLE_VALUE;}
};
struct UniqueSocket {
    SOCKET handle=INVALID_SOCKET;
    explicit UniqueSocket(SOCKET value=INVALID_SOCKET)noexcept:handle(value){}
    ~UniqueSocket(){if(handle!=INVALID_SOCKET)closesocket(handle);}
    UniqueSocket(const UniqueSocket&)=delete;UniqueSocket& operator=(const UniqueSocket&)=delete;
    SOCKET get()const noexcept{return handle;}
};
struct WinsockSession {
    bool active=false;
    explicit WinsockSession(WSADATA& data):active(WSAStartup(MAKEWORD(2,2),&data)==0){}
    ~WinsockSession(){if(active)WSACleanup();}
};
struct MappedDump {
    UniqueHandle file,mapping;
    void* view=nullptr;
    size_t length=0;
    explicit MappedDump(const fs::path& path)
        :file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)){
        if(!file.valid())throw std::runtime_error("Cannot open incident minidump.");
        LARGE_INTEGER bytes{};
        if(!GetFileSizeEx(file.get(),&bytes)||bytes.QuadPart<static_cast<LONGLONG>(sizeof(MINIDUMP_HEADER)))throw std::runtime_error("Truncated minidump header.");
        length=static_cast<size_t>(bytes.QuadPart);
        mapping.handle=CreateFileMappingW(file.get(),nullptr,PAGE_READONLY,0,0,nullptr);
        if(!mapping.valid())throw std::runtime_error("Cannot map incident minidump.");
        view=MapViewOfFile(mapping.get(),FILE_MAP_READ,0,0,0);
        if(!view)throw std::runtime_error("Cannot view incident minidump.");
        try{
            MINIDUMP_HEADER header{};std::memcpy(&header,view,sizeof(header));
            if(header.Signature!=MINIDUMP_SIGNATURE||header.StreamDirectoryRva>length||
               header.NumberOfStreams>(length-header.StreamDirectoryRva)/sizeof(MINIDUMP_DIRECTORY))throw std::runtime_error("Invalid minidump stream directory.");
            for(ULONG i=0;i<header.NumberOfStreams;++i){
                MINIDUMP_DIRECTORY entry{};std::memcpy(&entry,static_cast<const char*>(view)+header.StreamDirectoryRva+static_cast<size_t>(i)*sizeof(entry),sizeof(entry));
                if(entry.Location.Rva>length||entry.Location.DataSize>length-entry.Location.Rva)throw std::runtime_error("Minidump stream exceeds mapped file.");
            }
        }catch(...){UnmapViewOfFile(view);view=nullptr;throw;}
    }
    ~MappedDump(){if(view)UnmapViewOfFile(view);}
};
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::string& bytes){
    if(bytes.size()>MAXDWORD)throw StorageError("Report attachment exceeds Win32 write length.");
    UniqueHandle file(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!file.valid())throw StorageError("Cannot create report file.");
    DWORD count=0;const bool success=WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file.get());
    if(!success)throw StorageError("Cannot persist report file.");
}
std::string identity(const std::string& envelope){
    auto h=Json::parse(envelope.substr(0,envelope.find('\n')));
    auto id=h.value("event_id",std::string{});
    id.erase(std::remove(id.begin(),id.end(),'-'),id.end());
    if(id.size()!=32||id.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw std::runtime_error("Invalid event identity.");
    return id;
}
void extract(const fs::path& folder,const std::string& envelope){
    size_t offset=envelope.find('\n')+1;int attachment=0;
    while(offset<envelope.size()){
        const auto end=envelope.find('\n',offset);if(end==std::string::npos)break;
        const auto item=Json::parse(envelope.substr(offset,end-offset));offset=end+1;
        auto size=item.value("length",size_t{});
        if(!item.contains("length")){const auto next=envelope.find('\n',offset);size=(next==std::string::npos?envelope.size():next)-offset;}
        if(size>envelope.size()-offset)throw std::runtime_error("Truncated envelope.");
        const auto type=item.value("type",std::string{});
        if(type=="event")write(folder/"event.json",envelope.substr(offset,size));
        else if(type=="attachment"){
            const bool dump=item.value("attachment_type",std::string{})=="event.minidump";
            const auto name=item.value("filename",std::string{});
            const bool known=name=="session.json"||name=="player-build.json"||name=="emergency.txt"||name=="phase.txt";
            write(folder/(dump?"crash.dmp":known?name:"attachment-"+std::to_string(++attachment)+".bin"),envelope.substr(offset,size));
        }
        offset+=size;if(offset<envelope.size()&&envelope[offset]=='\n')++offset;
    }
}
void prune(const fs::path& incidents,const fs::path& keep){
    std::vector<fs::directory_entry> entries;uintmax_t bytes=0;
    for(const auto& e:fs::directory_iterator(incidents))if(e.is_directory()&&!e.is_symlink()){
        if(e.path()!=keep)entries.push_back(e);for(const auto& file:fs::directory_iterator(e.path()))if(file.is_regular_file())bytes+=file.file_size();
    }
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});
    for(size_t i=0;i<entries.size()&&(entries.size()+1-i>50||bytes>1024ULL*1024*1024);++i){
        uintmax_t size=0;for(const auto& f:fs::directory_iterator(entries[i].path()))if(f.is_regular_file())size+=f.file_size();
        fs::remove_all(entries[i].path());bytes-=std::min(bytes,size);
    }
}
void reply(SOCKET socket,int status,const std::string& body){
    const auto response="HTTP/1.1 "+std::to_string(status)+(status==200?" OK":" Error")+"\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    size_t sent=0;while(sent<response.size()){const auto n=send(socket,response.data()+sent,static_cast<int>(response.size()-sent),0);if(n<=0)break;sent+=n;}
}
int serve(const fs::path& root,unsigned short port){
    const auto name=L"Local\\PrappyReports-"+std::to_wstring(port);
    UniqueHandle singleton(CreateMutexW(nullptr,FALSE,(name+L"-mutex").c_str()));
    if(!singleton.valid())return 2;
    if(GetLastError()==ERROR_ALREADY_EXISTS)return 0;
    WSADATA data{};WinsockSession session(data);if(!session.active)return 3;
    UniqueSocket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    if(listener.get()==INVALID_SOCKET)return 4;
    BOOL exclusive=TRUE;
    setsockopt(listener.get(),SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener.get(),8))return 4;
    fs::create_directories(root/"incidents");
    UniqueHandle ready(CreateEventW(nullptr,TRUE,FALSE,(name+L"-ready").c_str()));
    if(!ready.valid())throw StorageError("Cannot create collector readiness event.");
    SetEvent(ready.get());
    bool stopping=false;std::string lastError;
    while(!stopping){
        UniqueSocket client(accept(listener.get(),nullptr,nullptr));if(client.get()==INVALID_SOCKET)break;
        DWORD timeout=5000;setsockopt(client.get(),SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        setsockopt(client.get(),SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        try{
            std::string request;char buffer[16384];size_t headers=std::string::npos;
            while((headers=request.find("\r\n\r\n"))==std::string::npos){
                const int n=recv(client.get(),buffer,sizeof(buffer),0);if(n<=0)throw std::runtime_error("Incomplete HTTP headers.");request.append(buffer,n);
                if(request.size()>32768)throw std::runtime_error("HTTP headers too large.");
            }
            if(request.starts_with("GET /health ")){const auto path=root.u8string();reply(client.get(),200,Json{{"root",std::string(path.begin(),path.end())},{"version",1},{"last_error",lastError}}.dump());continue;}
            if(request.starts_with("POST /shutdown ")){reply(client.get(),200,"{}");stopping=true;continue;}
            if(!request.starts_with("POST /api/1/envelope/ "))throw std::runtime_error("Unsupported collector endpoint.");
            auto head=request.substr(0,headers);std::transform(head.begin(),head.end(),head.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
            const auto marker=head.find("\r\ncontent-length:");if(marker==std::string::npos)throw std::runtime_error("Content-Length required.");
            const auto length=std::stoull(head.substr(marker+17));if(length>MaxEnvelope)throw std::runtime_error("Envelope too large.");
            std::string envelope=request.substr(headers+4);
            while(envelope.size()<length){const auto n=recv(client.get(),buffer,static_cast<int>(std::min(sizeof(buffer),length-envelope.size())),0);if(n<=0)throw std::runtime_error("Incomplete envelope.");envelope.append(buffer,n);}
            envelope.resize(static_cast<size_t>(length));
            const auto id=identity(envelope);const auto folder=root/"incidents"/id;fs::create_directories(folder);
            if(fs::exists(folder/"report.envelope")){
                if(read(folder/"report.envelope")!=envelope)throw std::runtime_error("Incident identity already belongs to different report bytes.");
                extract(folder,envelope);lastError.clear();reply(client.get(),200,Json{{"id",id}}.dump());continue;
            }
            // Acknowledge only after the exact envelope is safely on disk.
            write(folder/"report.envelope.tmp",envelope);
            if(!MoveFileExW((folder/"report.envelope.tmp").c_str(),(folder/"report.envelope").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw StorageError("Cannot commit envelope.");
            extract(folder,envelope);
            try{prune(root/"incidents",folder);}catch(...){/* retention must not reject an accepted incident */}
            lastError.clear();
            reply(client.get(),200,Json{{"id",id}}.dump());
        }catch(const StorageError& e){lastError=e.what();reply(client.get(),503,Json{{"error",lastError}}.dump());}
        catch(const fs::filesystem_error& e){lastError=e.what();reply(client.get(),503,Json{{"error",lastError}}.dump());}
        catch(const std::exception& e){reply(client.get(),400,Json{{"error",e.what()}}.dump());}
    }
    ResetEvent(ready.get());return 0;
}
struct Sink final:IDebugOutputCallbacks {
    bool quiet=true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        if(id==__uuidof(IUnknown)||id==__uuidof(IDebugOutputCallbacks)){*out=this;return S_OK;}
        *out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return 1;} ULONG STDMETHODCALLTYPE Release()override{return 1;}
    HRESULT STDMETHODCALLTYPE Output(ULONG,PCSTR value)override{if(!quiet)std::cout<<value;return S_OK;}
};
void failureLocation(IDebugControl* control,IDebugSymbols* symbols,const fs::path& dump){
    // A full-memory snapshot can be many gigabytes. Mapping it reserves a
    // virtual view instead of allocating and copying the entire dump to RAM.
    MappedDump bytes(dump);PMINIDUMP_DIRECTORY directory=nullptr;PVOID stream=nullptr;ULONG size=0;
    if(MiniDumpReadDumpStream(bytes.view,ExceptionStream,&directory,&stream,&size)&&size>=sizeof(MINIDUMP_EXCEPTION_STREAM)){
        MINIDUMP_EXCEPTION_STREAM exception{};std::memcpy(&exception,stream,sizeof(exception));
        std::cout<<"Fault: code=0x"<<std::hex<<exception.ExceptionRecord.ExceptionCode<<std::dec<<" thread="<<exception.ThreadId<<"\n";
        control->Execute(DEBUG_OUTCTL_IGNORE,".ecxr",DEBUG_EXECUTE_DEFAULT);
    }else std::cout<<"Live snapshot: no original exception context\n";
    DEBUG_STACK_FRAME frames[128]{};ULONG count=0;
    if(FAILED(control->GetStackTrace(0,0,0,frames,128,&count)))return;
    for(ULONG i=0;i<count;i++){
        char name[2048]{},file[4096]{};ULONG line=0;
        symbols->GetNameByOffset(frames[i].InstructionOffset,name,sizeof(name),nullptr,nullptr);
        if(FAILED(symbols->GetLineByOffset(frames[i].InstructionOffset,&line,file,sizeof(file),nullptr,nullptr)))continue;
        const std::string source=file,function=name;
        if(source.find("prappy-player\\src\\")==std::string::npos||function.find("reporting::assertion")!=std::string::npos||function.find("terminateHandler")!=std::string::npos)continue;
        std::cout<<"Failure location: "<<name<<" at "<<file<<":"<<line<<"\n";return;
    }
    std::cout<<"Failure location: no application source frame resolved; inspect module/PDB details below.\n";
}
void eventStacks(const Json& event,const fs::path& symbols,const std::string& search){
    const auto process=GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_EXACT_SYMBOLS);
    if(!SymInitialize(process,search.c_str(),FALSE))throw std::runtime_error("Cannot initialize local symbol resolver.");
    struct Cleanup {HANDLE process;~Cleanup(){SymCleanup(process);}} cleanup{process};
    std::string build;
    if(event.contains("tags"))build=event["tags"].value("build_id",std::string{});
    if(event.contains("debug_meta"))for(const auto& image:event["debug_meta"].value("images",Json::array())){
        const auto original=fs::path(image.value("code_file",std::string{}));
        const auto archived=symbols/build/original.filename();
        const auto path=fs::exists(archived)?archived:original;
        if(!fs::exists(path))continue;
        const auto base=std::stoull(image.value("image_addr",std::string("0")),nullptr,0);
        const auto size=image.value("image_size",DWORD{});
        SymLoadModuleExW(process,nullptr,path.c_str(),nullptr,base,size,nullptr,0);
    }
    auto print=[&](const Json& stack){
        auto frames=stack.value("frames",Json::array());
        for(auto i=frames.rbegin();i!=frames.rend();++i){
            const auto address=std::stoull(i->value("instruction_addr",std::string("0")),nullptr,0);
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+2048]{};
            auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=2047;
            DWORD64 displacement=0;std::cout<<"0x"<<std::hex<<address<<std::dec<<" ";
            if(SymFromAddr(process,address,&displacement,symbol))std::cout<<symbol->Name;
            else std::cout<<"[unresolved]";
            IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD offset=0;
            if(SymGetLineFromAddr64(process,address,&offset,&line))std::cout<<" at "<<line.FileName<<":"<<line.LineNumber;
            std::cout<<"\n";
        }
    };
    if(event.contains("exception"))for(const auto& value:event["exception"].value("values",Json::array()))if(value.contains("stacktrace"))print(value["stacktrace"]);
    if(event.contains("threads"))for(const auto& value:event["threads"].value("values",Json::array()))if(value.contains("stacktrace")){std::cout<<"Thread "<<value.value("id",Json{})<<"\n";print(value["stacktrace"]);}
}
int analyze(const fs::path& folder,const fs::path& root){
    const auto eventFile=folder/"event.json";
    Json event=fs::exists(eventFile)?Json::parse(read(eventFile)):Json::object();
    std::cout<<"Incident: "<<folder.filename().string()<<"\n";
    if(fs::exists(folder/"phase.txt"))std::cout<<read(folder/"phase.txt")<<"\n";
    if(event.contains("exception"))for(const auto& e:event["exception"]["values"])std::cout<<e.value("type","")<<": "<<e.value("value","")<<"\n";
    const auto symbols=root/"symbols";std::string search;
    if(fs::exists(symbols))for(const auto& entry:fs::directory_iterator(symbols))if(entry.is_directory()){if(!search.empty())search+=';';search+=entry.path().string();}
    Microsoft::WRL::ComPtr<IDebugClient> client;
    Microsoft::WRL::ComPtr<IDebugControl> control;
    Microsoft::WRL::ComPtr<IDebugSymbols> debugSymbols;
    if(FAILED(DebugCreate(__uuidof(IDebugClient),reinterpret_cast<void**>(client.GetAddressOf()))))
        throw std::runtime_error("Cannot initialize Windows debug engine.");
    if(FAILED(client->QueryInterface(__uuidof(IDebugControl),reinterpret_cast<void**>(control.GetAddressOf())))||
       FAILED(client->QueryInterface(__uuidof(IDebugSymbols),reinterpret_cast<void**>(debugSymbols.GetAddressOf()))))
        throw std::runtime_error("Windows debug engine lacks control or symbol interfaces.");
    Sink sink;
    if(FAILED(client->SetOutputCallbacks(&sink)))throw std::runtime_error("Cannot configure local debug output.");
    struct CallbackScope {IDebugClient* client;~CallbackScope(){client->SetOutputCallbacks(nullptr);}} callbackScope{client.Get()};
    debugSymbols->SetSymbolPath(search.c_str());
    debugSymbols->SetImagePath(search.c_str());
    HRESULT result=S_OK;
    if(fs::exists(folder/"crash.dmp")){
        MappedDump validated(folder/"crash.dmp");
        result=client->OpenDumpFile((folder/"crash.dmp").string().c_str());if(SUCCEEDED(result))result=control->WaitForEvent(0,INFINITE);
        if(SUCCEEDED(result)){
            control->Execute(DEBUG_OUTCTL_IGNORE,".lines -e",DEBUG_EXECUTE_DEFAULT);
            failureLocation(control.Get(),debugSymbols.Get(),folder/"crash.dmp");
            sink.quiet=false;
            result=control->Execute(DEBUG_OUTCTL_THIS_CLIENT,".ecxr; ~* kn; lm e; lmv m PrappyPlayer; lmv m prappy_crash_probe",DEBUG_EXECUTE_DEFAULT);
        }
    }else{
        // Handled exceptions already carry their throw-site trace, resolved before teardown.
        const auto fatalFile=folder/"fatal.json";
        if(fs::exists(fatalFile)){const auto fatal=Json::parse(read(fatalFile));std::cout<<fatal.dump(2)<<"\n";}
        else{eventStacks(event,symbols,search);std::cout<<"Event metadata:\n"<<event.dump(2)<<"\n";}
    }
    return FAILED(result)?2:0;
}
int snapshot(DWORD pid,const fs::path& folder){
    UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE,FALSE,pid));
    if(!process.valid())throw std::runtime_error("Cannot open snapshot target (Windows "+std::to_string(GetLastError())+").");
    fs::create_directories(folder);
    UniqueHandle file(CreateFileW((folder/"crash.dmp").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file.valid())throw std::runtime_error("Snapshot destination must not already contain a dump.");
    const auto flags=static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory|MiniDumpWithHandleData|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);
    const bool success=MiniDumpWriteDump(process.get(),pid,file.get(),flags,nullptr,nullptr,nullptr)!=FALSE;
    const auto error=success?ERROR_SUCCESS:GetLastError();FlushFileBuffers(file.get());
    wchar_t path[32768]{};DWORD length=32768;
    Json event={{"exception",{{"values",Json::array({{{"type","ManualSnapshot"},{"value","Live full-memory snapshot; no original exception context is implied."}}})}}},{"pid",pid}};
    if(QueryFullProcessImageNameW(process.get(),0,path,&length)){
        const auto manifest=fs::path(path).parent_path()/"player-build.json";
        if(fs::exists(manifest)){const auto build=Json::parse(read(manifest));event["tags"]={{"build_id",build.value("build_id",std::string{})}};write(folder/"player-build.json",build.dump(2));}
    }
    if(!success)throw std::runtime_error("Full snapshot failed (Windows "+std::to_string(error)+").");
    write(folder/"event.json",event.dump(2));std::cout<<"Full snapshot saved to "<<folder.string()<<"\n";return 0;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4&&std::wstring(argv[1])==L"--serve")return serve(fs::absolute(argv[2]),static_cast<unsigned short>(std::stoul(argv[3])));
        if(argc==4&&std::wstring(argv[1])==L"--snapshot")return snapshot(static_cast<DWORD>(std::stoul(argv[2])),fs::absolute(argv[3]));
        if(argc>=3&&std::wstring(argv[1])==L"--analyze"){
            const auto folder=fs::absolute(argv[2]);return analyze(folder,argc==4?fs::absolute(argv[3]):folder.parent_path().parent_path());
        }
        std::cerr<<"PrappyReports --serve ROOT PORT | --analyze INCIDENT [ROOT] | --snapshot PID NEW_FOLDER\n";return 2;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

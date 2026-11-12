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
#include <json.hpp>
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
constexpr size_t MaxEnvelope=64*1024*1024;
struct StorageError:std::runtime_error{using std::runtime_error::runtime_error;};
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::string& bytes){
    HANDLE file=CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw StorageError("Cannot create report file.");
    DWORD count=0;const bool success=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);if(!success)throw StorageError("Cannot persist report file.");
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
    HANDLE singleton=CreateMutexW(nullptr,FALSE,(name+L"-mutex").c_str());
    if(!singleton)return 2;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(singleton);return 0;}
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return 3;
    const auto listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);BOOL exclusive=TRUE;
    setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,8))return 4;
    fs::create_directories(root/"incidents");
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,(name+L"-ready").c_str());SetEvent(ready);
    bool stopping=false;std::string lastError;
    while(!stopping){
        const auto client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)break;
        DWORD timeout=5000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        try{
            std::string request;char buffer[16384];size_t headers=std::string::npos;
            while((headers=request.find("\r\n\r\n"))==std::string::npos){
                const int n=recv(client,buffer,sizeof(buffer),0);if(n<=0)throw std::runtime_error("Incomplete HTTP headers.");request.append(buffer,n);
                if(request.size()>32768)throw std::runtime_error("HTTP headers too large.");
            }
            if(request.starts_with("GET /health ")){const auto path=root.u8string();reply(client,200,Json{{"root",std::string(path.begin(),path.end())},{"version",1},{"last_error",lastError}}.dump());closesocket(client);continue;}
            if(request.starts_with("POST /shutdown ")){reply(client,200,"{}");stopping=true;closesocket(client);continue;}
            if(!request.starts_with("POST /api/1/envelope/ "))throw std::runtime_error("Unsupported collector endpoint.");
            auto head=request.substr(0,headers);std::transform(head.begin(),head.end(),head.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
            const auto marker=head.find("\r\ncontent-length:");if(marker==std::string::npos)throw std::runtime_error("Content-Length required.");
            const auto length=std::stoull(head.substr(marker+17));if(length>MaxEnvelope)throw std::runtime_error("Envelope too large.");
            std::string envelope=request.substr(headers+4);
            while(envelope.size()<length){const auto n=recv(client,buffer,static_cast<int>(std::min(sizeof(buffer),length-envelope.size())),0);if(n<=0)throw std::runtime_error("Incomplete envelope.");envelope.append(buffer,n);}
            envelope.resize(static_cast<size_t>(length));
            const auto id=identity(envelope);const auto folder=root/"incidents"/id;fs::create_directories(folder);
            if(fs::exists(folder/"report.envelope")){
                if(read(folder/"report.envelope")!=envelope)throw std::runtime_error("Incident identity already belongs to different report bytes.");
                extract(folder,envelope);lastError.clear();reply(client,200,Json{{"id",id}}.dump());closesocket(client);continue;
            }
            // Acknowledge only after the exact envelope is safely on disk.
            write(folder/"report.envelope.tmp",envelope);
            if(!MoveFileExW((folder/"report.envelope.tmp").c_str(),(folder/"report.envelope").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw StorageError("Cannot commit envelope.");
            extract(folder,envelope);
            try{prune(root/"incidents",folder);}catch(...){/* retention must not reject an accepted incident */}
            lastError.clear();
            reply(client,200,Json{{"id",id}}.dump());
        }catch(const StorageError& e){lastError=e.what();reply(client,503,Json{{"error",lastError}}.dump());}
        catch(const fs::filesystem_error& e){lastError=e.what();reply(client,503,Json{{"error",lastError}}.dump());}
        catch(const std::exception& e){reply(client,400,Json{{"error",e.what()}}.dump());}
        closesocket(client);
    }
    ResetEvent(ready);CloseHandle(ready);closesocket(listener);WSACleanup();CloseHandle(singleton);return 0;
}
struct Sink final:IDebugOutputCallbacks {
    bool quiet=true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(id==__uuidof(IUnknown)||id==__uuidof(IDebugOutputCallbacks)){*out=this;return S_OK;}*out=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return 1;} ULONG STDMETHODCALLTYPE Release()override{return 1;}
    HRESULT STDMETHODCALLTYPE Output(ULONG,PCSTR value)override{if(!quiet)std::cout<<value;return S_OK;}
};
void failureLocation(IDebugControl* control,IDebugSymbols* symbols,const fs::path& dump){
    const auto bytes=read(dump);PMINIDUMP_DIRECTORY directory=nullptr;PVOID stream=nullptr;ULONG size=0;
    if(MiniDumpReadDumpStream(const_cast<char*>(bytes.data()),ExceptionStream,&directory,&stream,&size)&&size>=sizeof(MINIDUMP_EXCEPTION_STREAM)){
        const auto* exception=static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
        std::cout<<"Fault: code=0x"<<std::hex<<exception->ExceptionRecord.ExceptionCode<<std::dec<<" thread="<<exception->ThreadId<<"\n";
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
    SymCleanup(process);
}
int analyze(const fs::path& folder,const fs::path& root){
    const auto eventFile=folder/"event.json";
    Json event=fs::exists(eventFile)?Json::parse(read(eventFile)):Json::object();
    std::cout<<"Incident: "<<folder.filename().string()<<"\n";
    if(fs::exists(folder/"phase.txt"))std::cout<<read(folder/"phase.txt")<<"\n";
    if(event.contains("exception"))for(const auto& e:event["exception"]["values"])std::cout<<e.value("type","")<<": "<<e.value("value","")<<"\n";
    const auto symbols=root/"symbols";std::string search;
    if(fs::exists(symbols))for(const auto& entry:fs::directory_iterator(symbols))if(entry.is_directory()){if(!search.empty())search+=';';search+=entry.path().string();}
    IDebugClient* client=nullptr;IDebugControl* control=nullptr;IDebugSymbols* debugSymbols=nullptr;Sink sink;
    if(FAILED(DebugCreate(__uuidof(IDebugClient),reinterpret_cast<void**>(&client))))throw std::runtime_error("Cannot initialize Windows debug engine.");
    client->SetOutputCallbacks(&sink);client->QueryInterface(__uuidof(IDebugControl),reinterpret_cast<void**>(&control));
    client->QueryInterface(__uuidof(IDebugSymbols),reinterpret_cast<void**>(&debugSymbols));debugSymbols->SetSymbolPath(search.c_str());
    debugSymbols->SetImagePath(search.c_str());
    HRESULT result=S_OK;
    if(fs::exists(folder/"crash.dmp")){
        result=client->OpenDumpFile((folder/"crash.dmp").string().c_str());if(SUCCEEDED(result))result=control->WaitForEvent(0,INFINITE);
        if(SUCCEEDED(result)){
            control->Execute(DEBUG_OUTCTL_IGNORE,".lines -e",DEBUG_EXECUTE_DEFAULT);
            failureLocation(control,debugSymbols,folder/"crash.dmp");
            sink.quiet=false;
            result=control->Execute(DEBUG_OUTCTL_THIS_CLIENT,".ecxr; ~* kn; lm e; lmv m PrappyPlayer; lmv m prappy_crash_probe",DEBUG_EXECUTE_DEFAULT);
        }
    }else{
        // Handled exceptions already carry their throw-site trace, resolved before teardown.
        const auto fatalFile=folder/"fatal.json";
        if(fs::exists(fatalFile)){const auto fatal=Json::parse(read(fatalFile));std::cout<<fatal.dump(2)<<"\n";}
        else{eventStacks(event,symbols,search);std::cout<<"Event metadata:\n"<<event.dump(2)<<"\n";}
    }
    debugSymbols->Release();control->Release();client->Release();return FAILED(result)?2:0;
}
int snapshot(DWORD pid,const fs::path& folder){
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE,FALSE,pid);
    if(!process)throw std::runtime_error("Cannot open snapshot target (Windows "+std::to_string(GetLastError())+").");
    fs::create_directories(folder);
    HANDLE file=CreateFileW((folder/"crash.dmp").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){CloseHandle(process);throw std::runtime_error("Snapshot destination must not already contain a dump.");}
    const auto flags=static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory|MiniDumpWithHandleData|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);
    const bool success=MiniDumpWriteDump(process,pid,file,flags,nullptr,nullptr,nullptr)!=FALSE;
    const auto error=GetLastError();FlushFileBuffers(file);CloseHandle(file);
    wchar_t path[32768]{};DWORD length=32768;
    Json event={{"exception",{{"values",Json::array({{{"type","ManualSnapshot"},{"value","Live full-memory snapshot; no original exception context is implied."}}})}}},{"pid",pid}};
    if(QueryFullProcessImageNameW(process,0,path,&length)){
        const auto manifest=fs::path(path).parent_path()/"player-build.json";
        if(fs::exists(manifest)){const auto build=Json::parse(read(manifest));event["tags"]={{"build_id",build.value("build_id",std::string{})}};write(folder/"player-build.json",build.dump(2));}
    }
    CloseHandle(process);
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

#include "crash_reporting.h"
#include "debug_log.h"
#include <sentry.h>
#include <boost/stacktrace.hpp>
#include <boost/stacktrace/this_thread.hpp>
#include <json.hpp>
#include <atomic>
#include <fstream>
#include <sstream>
#include <typeinfo>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <intrin.h>
#include <winhttp.h>
#include <deque>
#include <condition_variable>
#include <cstring>
#include <utility>
namespace reporting {
namespace {
using Json=nlohmann::json;
fs::path root,session,executablePath;
std::atomic<bool> ready=false,reported=false;
bool dialogs=true;
HANDLE emergency=INVALID_HANDLE_VALUE;
HANDLE phaseFile=INVALID_HANDLE_VALUE,phaseMapping=nullptr;
char* phaseView=nullptr;
DWORD phaseOwner=0;
thread_local const char* operation="startup";
std::string buildIdentity,sessionIdentity;
std::mutex breadcrumbMutex;
std::condition_variable breadcrumbChanged;
std::deque<std::pair<std::string,std::string>> breadcrumbs;
std::thread breadcrumbWorker;
bool breadcrumbStopping=false,breadcrumbWriting=false;
size_t breadcrumbDropped=0;
void emitBreadcrumb(const std::string& category,const std::string& message){
    auto crumb=sentry_value_new_breadcrumb("default",message.c_str());
    sentry_value_set_by_key(crumb,"category",sentry_value_new_string(category.c_str()));
    sentry_add_breadcrumb(crumb);
}
void runBreadcrumbs(){
    for(;;){
        std::pair<std::string,std::string> row;size_t lost=0;
        {
            std::unique_lock lock(breadcrumbMutex);
            breadcrumbChanged.wait(lock,[]{return breadcrumbStopping||!breadcrumbs.empty();});
            if(breadcrumbs.empty()&&breadcrumbStopping)return;
            row=std::move(breadcrumbs.front());breadcrumbs.pop_front();breadcrumbWriting=true;
            lost=std::exchange(breadcrumbDropped,0);
        }
        try{if(lost)emitBreadcrumb("diagnostics.dropped",std::to_string(lost));emitBreadcrumb(row.first,row.second);}catch(...){}
        {std::lock_guard lock(breadcrumbMutex);breadcrumbWriting=false;}
        breadcrumbChanged.notify_all();
    }
}
void pruneSessions(){
    std::vector<fs::directory_entry> completed;
    for(const auto& entry:fs::directory_iterator(root/"sessions")){
        if(!entry.is_directory()||entry.is_symlink()||entry.path()==session)continue;
        if(!fs::exists(entry.path()/"exit.json")){
            // Do not remove another live player's session. Crashed runs are
            // eligible too, once their native report has had time to arrive.
            if(fs::file_time_type::clock::now()-entry.last_write_time()<std::chrono::minutes(5))continue;
            const auto info=entry.path()/"session.json";
            std::ifstream input(info);Json identity;input>>identity;
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,identity.at("pid").get<DWORD>());
            if(process){const bool active=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);if(active)continue;}
        }
        completed.push_back(entry);
    }
    std::sort(completed.begin(),completed.end(),[](const auto& a,const auto& b){return a.last_write_time()>b.last_write_time();});
    for(size_t i=100;i<completed.size();++i)fs::remove_all(completed[i].path());
}
std::string uuid(){auto id=sentry_uuid_new_v4();char value[37]{};sentry_uuid_as_string(&id,value);std::string result=value;result.erase(std::remove(result.begin(),result.end(),'-'),result.end());return result;}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void persist(const fs::path& p,const std::string& bytes){
    const auto file=CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create incident file (Windows "+std::to_string(GetLastError())+").");
    DWORD count=0;const bool success=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);if(!success)throw std::runtime_error("Cannot persist incident file.");
}
void warning(const char* message)noexcept{
    OutputDebugStringA(message);
    if(dialogs)MessageBoxA(nullptr,message,"Prappy: crash reporting unavailable",MB_OK|MB_ICONWARNING);
    else std::fprintf(stderr,"%s\n",message);
}
void emergencyWrite(const char* message,DWORD length)noexcept{
    if(emergency!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(emergency,message,length,&written,nullptr);FlushFileBuffers(emergency);}
}
void launch(const fs::path& program,std::wstring arguments){
    std::wstring command=L"\""+program.wstring()+L"\" "+arguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,program.parent_path().c_str(),&startup,&process))
        throw std::runtime_error("Cannot start local report collector (Windows "+std::to_string(GetLastError())+").");
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
bool collectorMatches(unsigned short port){
    HINTERNET http=WinHttpOpen(L"PrappyReports/1",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0);
    if(!http)return false;
    WinHttpSetTimeouts(http,1000,1000,1000,1000);
    HINTERNET connection=WinHttpConnect(http,L"127.0.0.1",port,0);
    HINTERNET request=connection?WinHttpOpenRequest(connection,L"GET",L"/health",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;
    bool matches=false;
    if(request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr)){
        char buffer[4096]{};DWORD bytes=0;
        if(WinHttpReadData(request,buffer,sizeof(buffer)-1,&bytes)){
            try{const auto response=Json::parse(std::string(buffer,bytes));const auto text=response.at("root").get<std::string>();matches=key(fs::absolute(fs::path(std::u8string(text.begin(),text.end()))))==key(root);}catch(...){}
        }
    }
    if(request)WinHttpCloseHandle(request);if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(http);return matches;
}
void terminateHandler()noexcept{
    // Let the SDK's external handler capture a genuine terminal failure. Do not
    // call the ordinary logger, unwind objects, allocate or symbolize here.
    static constexpr char message[]="std::terminate called; inspect the external crash dump.\r\n";
    emergencyWrite(message,sizeof(message)-1);
    RaiseException(0xE0425053,EXCEPTION_NONCONTINUABLE,0,nullptr);
    TerminateProcess(GetCurrentProcess(),3);std::abort();
}
Json metadata(){SYSTEMTIME t{};GetSystemTime(&t);char stamp[40]{};std::snprintf(stamp,sizeof(stamp),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    return Json{{"utc",stamp},{"pid",GetCurrentProcessId()},{"tid",GetCurrentThreadId()},{"build_id",buildIdentity},{"session_id",sessionIdentity},{"operation",operation}};
}
void chain(std::exception_ptr pointer,Json& values,int depth=0){
    if(depth>=16){values.push_back({{"message","Nested exception depth limit"}});return;}
    try{if(pointer)std::rethrow_exception(pointer);else throw 0;}
    catch(const std::exception& e){
        values.push_back({{"type",typeid(e).name()},{"message",e.what()}});
        try{std::rethrow_if_nested(e);}catch(...){chain(std::current_exception(),values,depth+1);}
    }catch(...){values.push_back({{"type","non-std C++ exception"},{"message","No std::exception::what() available"}});}
}
}
void initialize(const fs::path& executable,const fs::path& reportRoot,bool interactive){
    dialogs=interactive;
    executablePath=fs::absolute(executable);
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
    wchar_t oldSearch[32768]{};GetEnvironmentVariableW(L"_NT_SYMBOL_PATH",oldSearch,32768);
    const auto symbolSearch=fs::path(module).parent_path().wstring()+L";"+executable.parent_path().wstring()+L";"+oldSearch;
    SetEnvironmentVariableW(L"_NT_SYMBOL_PATH",symbolSearch.c_str());
    if(reportRoot.empty()){
        wchar_t local[32768]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768))throw std::runtime_error("LOCALAPPDATA is unavailable for fatal reporting.");
        root=fs::path(local)/L"Prappy"/L"Reports";
    }else root=fs::absolute(reportRoot);
    sessionIdentity=uuid();session=root/"sessions"/sessionIdentity;fs::create_directories(session);
    try{pruneSessions();}catch(...){/* retain evidence if cleanup fails */}
    const auto manifest=executable.parent_path()/"player-build.json";
    if(fs::exists(manifest)){
        const auto build=Json::parse(read(manifest));buildIdentity=build.value("build_id",build.value("commit",std::string("development")));
        persist(session/"player-build.json",build.dump(2));
    }else buildIdentity="development";
    persist(session/"session.json",metadata().dump(2));
    emergency=CreateFileW((session/"emergency.txt").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(emergency==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open emergency fatal report.");
    phaseFile=CreateFileW((session/"phase.txt").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(phaseFile==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create live phase context.");
    phaseMapping=CreateFileMappingW(phaseFile,nullptr,PAGE_READWRITE,0,256,nullptr);
    phaseView=phaseMapping?static_cast<char*>(MapViewOfFile(phaseMapping,FILE_MAP_WRITE,0,0,256)):nullptr;
    if(!phaseView)throw std::runtime_error("Cannot map live phase context.");
    phaseOwner=GetCurrentThreadId();phase("startup");
    const unsigned short port=reportRoot.empty()?19876:19877;
    const auto readyName=L"Local\\PrappyReports-"+std::to_wstring(port)+L"-ready";
    HANDLE collectorReady=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str());
    if(!collectorReady)throw std::runtime_error("Cannot create collector readiness event.");
    try{launch(executable.parent_path()/L"PrappyReports.exe",L"--serve \""+root.wstring()+L"\" "+std::to_wstring(port));}
    catch(...){CloseHandle(collectorReady);throw;}
    const auto status=WaitForSingleObject(collectorReady,5000);CloseHandle(collectorReady);
    if(status!=WAIT_OBJECT_0)throw std::runtime_error("Local report collector did not become ready.");
    if(!collectorMatches(port))throw std::runtime_error("Collector port belongs to a different report store.");
    auto* options=sentry_options_new();
    sentry_options_set_debug(options,0);
    const auto dsn="http://prappy@127.0.0.1:"+std::to_string(port)+"/1";
    sentry_options_set_dsn(options,dsn.c_str());
    sentry_options_set_database_pathw(options,(root/L"sdk-cache").c_str());
    sentry_options_set_handler_pathw(options,(executable.parent_path()/L"sentry-crash.exe").c_str());
    sentry_options_set_release(options,buildIdentity.c_str());
    sentry_options_set_auto_session_tracking(options,0);
    sentry_options_set_enable_app_hang_tracking(options,1);
    sentry_options_set_app_hang_timeout(options,15000);
    sentry_options_set_max_breadcrumbs(options,128);
    sentry_options_set_shutdown_timeout(options,2000);
    sentry_options_set_cache_keep(options,SENTRY_CACHE_KEEP_OFFLINE);
    sentry_options_set_cache_max_items(options,50);
    sentry_options_set_cache_max_size(options,256*1024*1024);
    sentry_options_set_crash_reporting_mode(options,SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);
    sentry_options_set_crash_upload_mode(options,SENTRY_CRASH_UPLOAD_MODE_SYNC);
    sentry_options_add_attachmentw(options,(session/"session.json").c_str());
    sentry_options_add_attachmentw(options,(session/"emergency.txt").c_str());
    sentry_options_add_attachmentw(options,(session/"phase.txt").c_str());
    if(fs::exists(session/"player-build.json"))sentry_options_add_attachmentw(options,(session/"player-build.json").c_str());
    if(sentry_init(options)!=0)throw std::runtime_error("External crash handler failed to initialize.");
    sentry_set_tag("session_id",sessionIdentity.c_str());sentry_set_tag("build_id",buildIdentity.c_str());
    std::set_terminate(terminateHandler);ready=true;
    breadcrumbWorker=std::thread(runBreadcrumbs);
    debuglog::setObserver(breadcrumb);
    breadcrumb("app.start",utf8(executable));
}
void phase(const char* value)noexcept{
    operation=value;
    // SDK scope mutation writes its database synchronously. A mapped attachment
    // preserves the UI's latest phase without allocating, locking or disk APIs
    // on each frame. The OS flushes mapped pages; the handler reads shared cache.
    if(phaseView&&GetCurrentThreadId()==phaseOwner){
        const int count=std::snprintf(phaseView,256,"last UI phase: %.160s; thread=%lu",value,phaseOwner);
        const size_t length=count>0?std::min<size_t>(count,255):0;
        std::memset(phaseView+length,' ',255-length);phaseView[255]='\n';
    }
}
void heartbeat()noexcept{if(ready&&!reported)sentry_app_hang_heartbeat();}
void pauseHang()noexcept{if(ready)sentry_app_hang_pause();}
void breadcrumb(const std::string& category,const std::string& message)noexcept{
    if(!ready||reported)return;
    try{
        auto row=std::make_pair(category.substr(0,128),message.substr(0,1024));
        {std::lock_guard lock(breadcrumbMutex);
            if(breadcrumbStopping)return;
            if(breadcrumbs.size()==128){breadcrumbs.pop_front();++breadcrumbDropped;}
            breadcrumbs.push_back(std::move(row));
        }
        breadcrumbChanged.notify_one();
    }catch(...){/* diagnostics must never break normal playback */}
}
void fatal(const char* handler,std::exception_ptr error)noexcept{
    if(ready)sentry_app_hang_pause();
    if(reported.exchange(true))return;
    try{
        const auto id=uuid();const auto folder=root/"incidents"/id;fs::create_directories(folder);
        auto record=metadata();record["incident_id"]=id;record["category"]="handled-fatal";record["handler"]=handler;
        // Capture BEFORE rethrow_exception/chain inspection: MSVC's exception_ptr
        // rethrow creates a new dispatch and otherwise replaces the original trace.
        auto trace=boost::stacktrace::stacktrace::from_current_exception();
        record["stack_origin"]=trace.empty()?"catch-site fallback":"initial throw";
        if(trace.empty())trace=boost::stacktrace::stacktrace();
        record["exceptions"]=Json::array();chain(error,record["exceptions"]);
        record["raw_stack"]=Json::array();for(const auto& frame:trace)record["raw_stack"].push_back(reinterpret_cast<uintptr_t>(frame.address()));
        record["stacktrace"]="Symbolization pending; raw throw-site addresses preserved.";
        persist(folder/"fatal.json",record.dump(2));
        record["stacktrace"]=boost::stacktrace::to_string(trace);
        persist(folder/"fatal.json",record.dump(2));
        if(!ready){warning("Fatal error was saved locally, but the external reporter was not initialized.");return;}
        {std::unique_lock lock(breadcrumbMutex);breadcrumbChanged.wait_for(lock,std::chrono::milliseconds(500),[]{return breadcrumbs.empty()&&!breadcrumbWriting;});}
        auto event=sentry_value_new_event();sentry_value_set_by_key(event,"event_id",sentry_value_new_string(id.c_str()));
        sentry_event_set_level(event,SENTRY_LEVEL_FATAL);
        const auto& primary=record["exceptions"][0];
        auto exception=sentry_value_new_exception(primary["type"].get<std::string>().c_str(),primary["message"].get<std::string>().c_str());
        std::vector<void*> frames;for(const auto& frame:trace)frames.push_back(const_cast<void*>(frame.address()));
        if(!frames.empty())sentry_value_set_stacktrace(exception,frames.data(),frames.size());
        sentry_event_add_exception(event,exception);
        sentry_value_set_by_key(event,"extra",sentry_value_new_object());

        sentry_capture_event(event);sentry_flush(2000);
    }catch(...){static constexpr char message[]="Fatal reporting failed; inspect any native dump and disk permissions.\r\n";emergencyWrite(message,sizeof(message)-1);warning("Prappy could not save its fatal report. Check disk space and permissions in LocalAppData\\Prappy\\Reports.");}
}
void finish(int code)noexcept{
    debuglog::setObserver(nullptr);
    const bool initialized=ready.exchange(false);
    {std::lock_guard lock(breadcrumbMutex);breadcrumbStopping=true;}
    breadcrumbChanged.notify_all();if(breadcrumbWorker.joinable())breadcrumbWorker.join();
    try{if(!session.empty()){auto record=metadata();record["exit_code"]=code;record["reason"]=reported?"handled-fatal":"normal";persist(session/"exit.json",record.dump(2));}}
    catch(...){warning("Prappy could not persist its exit status.");}
    if(initialized)sentry_close();
    if(phaseView){UnmapViewOfFile(phaseView);phaseView=nullptr;}
    if(phaseMapping){CloseHandle(phaseMapping);phaseMapping=nullptr;}
    if(phaseFile!=INVALID_HANDLE_VALUE){CloseHandle(phaseFile);phaseFile=INVALID_HANDLE_VALUE;}
    if(emergency!=INVALID_HANDLE_VALUE){CloseHandle(emergency);emergency=INVALID_HANDLE_VALUE;}
}
[[noreturn]] void assertion(const char* expression,const char* file,int line)noexcept{
    char message[1024]{};const int count=std::snprintf(message,sizeof(message),"Assertion failure: %.500s at %.400s:%d\r\n",expression,file,line);
    emergencyWrite(message,static_cast<DWORD>(std::clamp(count,0,static_cast<int>(sizeof(message)-1))));
    // SDK breadcrumbs include assertion identity in the external report.
    try{breadcrumb("assertion",std::string(file)+":"+std::to_string(line)+" "+expression);}catch(...){}
    RaiseException(0xE0425052,EXCEPTION_NONCONTINUABLE,0,nullptr);TerminateProcess(GetCurrentProcess(),3);std::abort();
}
void imguiError(const char* message,const char* window,int frame)noexcept{
    try{
        const auto detail=std::string("frame=")+std::to_string(frame)+" window="+(window?window:"NULL")+" "+message;
        breadcrumb("imgui.error",detail);
        // Persist the first error of each frame, capped per run to avoid an
        // invalid Begin/End loop flooding the collector.
        static int previous=-1;static unsigned reports=0;
        if(ready&&frame!=previous&&reports<8){previous=frame;++reports;
            auto event=sentry_value_new_message_event(SENTRY_LEVEL_ERROR,"imgui",detail.c_str());sentry_capture_event(event);
        }
    }catch(...){}
}
__declspec(noinline) void probeThrowSite(){throw std::runtime_error("Injected handled exception for crash reporting verification.");}
void probe(std::string_view kind){
    phase("crash-probe");
    if(kind=="handled"||kind=="handled-exception")probeThrowSite();
    if(kind=="third-party"){Json::object().at("missing");}
    if(kind=="unknown")throw 42;
    if(kind=="nested"){try{probeThrowSite();}catch(...){std::throw_with_nested(std::runtime_error("Outer injected failure"));}}
    if(kind=="access-violation"){
        auto* page=static_cast<volatile int*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READONLY));if(!page)throw std::bad_alloc();*page=1;
    }
    if(kind=="assert")assertion("injected assertion",__FILE__,__LINE__);
    if(kind=="abort"){_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);std::abort();}
    if(kind=="terminate")std::terminate();
    if(kind=="worker"){std::thread thread([]{throw std::runtime_error("Injected worker failure");});thread.join();}
    if(kind=="imgui")imguiError("Injected recoverable ImGui error","CrashProbe",1);
    if(kind=="modal"){heartbeat();pauseHang();Sleep(16000);heartbeat();}
    if(kind=="phase-load"){
        {std::unique_lock lock(breadcrumbMutex);breadcrumbChanged.wait_for(lock,std::chrono::seconds(2),[]{return breadcrumbs.empty()&&!breadcrumbWriting;});}
        sentry_flush(1000);
        IO_COUNTERS before{},after{};GetProcessIoCounters(GetCurrentProcess(),&before);
        for(int i=0;i<100000;++i)phase("phase-load");
        GetProcessIoCounters(GetCurrentProcess(),&after);
        const auto writes=after.WriteOperationCount-before.WriteOperationCount;
        persist(root/"phase-io.json",Json{{"phase_updates",100000},{"write_operations",writes}}.dump(2));
        if(writes)throw std::runtime_error("UI phase updates performed file I/O.");
    }
    if(kind=="hang"){heartbeat();Sleep(INFINITE);}
}
}

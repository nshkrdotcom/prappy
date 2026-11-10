#pragma once
#include "debug_log.h"
#include <cstdlib>
#include <vector>

namespace debugcrash {
inline void start(const fs::path& executableDirectory, bool probe) {
#ifdef _DEBUG
    if (!IsDebuggerPresent()) {
        const auto tool=executableDirectory/L"procdump64.exe";
        if (!fs::is_regular_file(tool)) throw std::runtime_error("Debug crash monitor missing; rebuild using scripts/build.ps1.");
        wchar_t local[MAX_PATH]{};
        const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        if (!length || length>=MAX_PATH) throw std::runtime_error("Cannot locate local crash dump directory.");
        const auto folder=fs::path(local)/L"Prappy"/L"CrashDumps";
        fs::create_directories(folder);
        std::vector<fs::directory_entry> dumps;
        for (const auto& item:fs::directory_iterator(folder))
            if(item.is_regular_file() && !item.is_symlink() && item.path().extension()==L".dmp" && item.path().filename().wstring().starts_with(L"PrappyPlayer")) dumps.push_back(item);
        std::sort(dumps.begin(),dumps.end(),[](const auto& a,const auto& b){return a.last_write_time()>b.last_write_time();});
        for(size_t i=2;i<dumps.size();++i)fs::remove(dumps[i]); // two previous dumps plus this run
        // CRT abort can exit without raising an unhandled exception. Capture
        // termination too; this also produces a small dump on a normal close.
        auto command=L"\""+tool.wstring()+L"\" -accepteula -mm -e -t -n 1 "+std::to_wstring(GetCurrentProcessId())+L" \""+folder.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(tool.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,executableDirectory.c_str(),&startup,&process))
            throw std::runtime_error("Cannot start Debug crash monitor: "+std::to_string(GetLastError()));
        CloseHandle(process.hThread);
        const auto deadline=GetTickCount64()+5000;
        while(!IsDebuggerPresent() && GetTickCount64()<deadline && WaitForSingleObject(process.hProcess,10)==WAIT_TIMEOUT) {}
        CloseHandle(process.hProcess);
        if(!IsDebuggerPresent())throw std::runtime_error("Debug crash monitor did not attach.");
        debuglog::write("crash.monitor","attached dump_directory="+utf8(folder));
    }
    debuglog::write("app.build",std::string("configuration=Debug compiled=")+__DATE__+" "+__TIME__+" executable_directory="+utf8(executableDirectory));
    debuglog::instance().flush();
    if(probe) {
        wchar_t kind[32]{};GetEnvironmentVariableW(L"PRAPPY_TEST_CRASH",kind,32);
        if(std::wstring(kind)==L"access-violation") {
            auto page=static_cast<volatile int*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READONLY));
            if(!page)throw std::runtime_error("Cannot allocate crash probe page.");
            *page=1;
        }
        if(std::wstring(kind)==L"abort") {
            _set_abort_behavior(0,_WRITE_ABORT_MSG);
            std::abort();
        }
    }
#else
    (void)executableDirectory;(void)probe;
#endif
}
}

#include "player_ui.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iterator>
#include "debug_leak_report.h"
#include "debug_log.h"
#include "crash_reporting.h"
#include <shellapi.h>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    bool windowTest=false;
    try {
        int count=0;auto* arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        if(arguments){windowTest=count>=2&&std::wstring(arguments[1])==L"--window-test";LocalFree(arguments);}
        const auto directory=executableDir();
        reporting::initialize(directory/L"PrappyPlayer.exe",windowTest?directory.parent_path()/L"reporting-validation":fs::path{},!windowTest);
        const int code=runPlayer(instance);reporting::finish(code);return code;
    }
    catch (const std::exception& e) {
        reporting::fatal("wWinMain");
        debuglog::write("app.fatal",std::string("handler=wWinMain error=")+e.what());
        if(!windowTest)MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR);
        else std::fprintf(stderr,"%s\n",e.what());
        reporting::finish(1);
        return 1;
    }
    catch (...) {
        reporting::fatal("wWinMain");reporting::finish(1);
        if(!windowTest)MessageBoxA(nullptr,"Unexpected non-standard C++ exception. See the local incident report.","Prappy Player",MB_OK|MB_ICONERROR);return 1;
    }
}

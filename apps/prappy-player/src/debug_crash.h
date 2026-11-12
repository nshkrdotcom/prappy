#pragma once
#include "debug_log.h"
#include "crash_reporting.h"
namespace debugcrash {
inline void start(const fs::path& directory,bool probe){
    debuglog::write("app.build",std::string("compiled=")+__DATE__+" "+__TIME__+" executable_directory="+utf8(directory));
    if(probe){wchar_t kind[32]{};GetEnvironmentVariableW(L"PRAPPY_TEST_CRASH",kind,32);reporting::probe(utf8(fs::path(kind)));}
}
}

#include "crash_reporting.h"
#include "debug_log.h"
#include <iostream>
#include <string>
#include <cstdlib>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
int wmain(int argc,wchar_t** argv){
    if(argc!=4)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
#endif
    try{
        reporting::initialize(fs::absolute(argv[1]),fs::absolute(argv[2]),false);
        debuglog::instance().configure(fs::absolute(argv[2])/"disabled-debug.log",false);
        debuglog::write("probe.breadcrumb","Recorded even with debug_log=false");
        reporting::probe(utf8(fs::path(argv[3])));
        reporting::finish(0);return 0;
    }catch(...){reporting::fatal("crash-probe");reporting::finish(1);return 1;}
}

#include "tests.h"
#include "sample_tests.h"
#include "paths.h"
#include "debug_leak_report.h"
#include <SDL3/SDL.h>
#include <objbase.h>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    std::cout<<"State regression build "<<__DATE__<<' '<<__TIME__<<'\n';
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Lifetime{HRESULT com;~Lifetime(){SDL_Quit();if(SUCCEEDED(com))CoUninitialize();}} lifetime{com};
    try{
        if(argc!=4)throw std::runtime_error("Usage: PrappyStateTests --state-test|--meter-test|--sampling-test input log");
        const std::wstring mode=argv[1];
        if(mode==L"--state-test")return stateTests(argv[2],argv[3]);
        if(mode==L"--meter-test")return outputMeterTests(argv[2],argv[3]);
        if(mode==L"--sampling-test")return sampling::integrationTest(argv[2],argv[3]);
        throw std::runtime_error("Unknown regression test mode.");
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

#pragma once
#include "paths.h"
#include <cstring>
#include <iterator>
#ifdef _DEBUG
#include <crtdbg.h>

// Construct before application globals so the report runs after their destruction.
struct DebugLeakReport {
    ~DebugLeakReport() {
        const auto capacity=GetEnvironmentVariableW(L"PRAPPY_LEAK_REPORT",nullptr,0);
        if(!capacity){_CrtDumpMemoryLeaks();return;}
        // Keep shutdown's stack small without adding a live CRT allocation to
        // the leak report that is about to inspect the CRT heap.
        auto* path=static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,static_cast<SIZE_T>(capacity)*sizeof(wchar_t)));
        if(!path){OutputDebugStringA("Cannot allocate leak-report path.\n");return;}
        const auto length=GetEnvironmentVariableW(L"PRAPPY_LEAK_REPORT",path,capacity);
        if(!length||length>=capacity){HeapFree(GetProcessHeap(),0,path);return;}
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HeapFree(GetProcessHeap(),0,path);
        if (file == INVALID_HANDLE_VALUE) { OutputDebugStringW(L"Cannot write PRAPPY_LEAK_REPORT.\n"); return; }
        const auto mode = _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        const auto previous = _CrtSetReportFile(_CRT_WARN, file);
        const bool leaked = _CrtDumpMemoryLeaks() != FALSE;
        const char* result = leaked ? "FAIL CRT leaks\r\n" : "PASS no CRT leaks\r\n";
        DWORD written = 0;
        WriteFile(file, result, static_cast<DWORD>(std::strlen(result)), &written, nullptr);
        _CrtSetReportFile(_CRT_WARN, previous);
        _CrtSetReportMode(_CRT_WARN, mode);
        CloseHandle(file);
    }
};
static DebugLeakReport debugLeakReport;
#endif

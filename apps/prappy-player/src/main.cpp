#include "player_ui.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iterator>
#ifdef _DEBUG
#include <crtdbg.h>

// Construct before application globals so the report runs after their destruction.
struct DebugLeakReport {
    ~DebugLeakReport() {
        wchar_t path[32768]{};
        const auto length = GetEnvironmentVariableW(L"PRAPPY_LEAK_REPORT", path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) { _CrtDumpMemoryLeaks(); return; }
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    try { return runPlayer(instance); }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR);
        return 1;
    }
}

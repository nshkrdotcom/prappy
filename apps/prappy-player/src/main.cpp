#include "player_ui.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iterator>
#include "debug_leak_report.h"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    try { return runPlayer(instance); }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Prappy Player", MB_OK | MB_ICONERROR);
        return 1;
    }
}

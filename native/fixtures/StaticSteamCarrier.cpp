#include <Windows.h>

extern "C" __declspec(dllimport) void* __cdecl FakeSteamFactory(
    const char* version) noexcept;

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH
        && FakeSteamFactory("SteamUser023") != nullptr) {
        // The fixture proves a static protected dependency can be used before
        // LoadLibrary returns. A correct preflight never executes this branch.
        ExitProcess(91);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl
StaticSteamCarrierReady() noexcept {
    return true;
}

extern "C" __declspec(dllexport) void* __cdecl
StaticSteamCarrierFactory(const char*) noexcept {
    return nullptr;
}

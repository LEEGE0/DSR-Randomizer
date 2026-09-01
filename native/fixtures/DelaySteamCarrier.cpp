#include <Windows.h>

extern "C" __declspec(dllimport) void* __cdecl FakeSteamFactory(
    const char* version) noexcept;

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH
        && FakeSteamFactory("SteamUser023") != nullptr) {
        ExitProcess(93);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl
DelaySteamCarrierReady() noexcept {
    return true;
}

extern "C" __declspec(dllexport) void* __cdecl
DelaySteamCarrierFactory(const char*) noexcept {
    return nullptr;
}

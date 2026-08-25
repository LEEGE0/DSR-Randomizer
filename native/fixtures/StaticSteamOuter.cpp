#include <Windows.h>

extern "C" __declspec(dllimport) bool __cdecl
StaticSteamCarrierReady() noexcept;

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH && StaticSteamCarrierReady()) {
        // The protected dependency's carrier must never be published through
        // an unprotected intermediate import closure.
        ExitProcess(92);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl
StaticSteamOuterReady() noexcept {
    return true;
}

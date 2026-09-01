#include <Windows.h>

#include <iterator>

namespace {

void LoadProtectedDependency() noexcept {
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(
        L"DSR_RANDOMIZER_SYNTHETIC_BRIDGE_TARGET",
        path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return;
    }
    const HMODULE module = LoadLibraryW(path);
    if (module != nullptr) {
        ExitProcess(77);
    }
}

}  // namespace

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LoadProtectedDependency();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl NestedSteamBridgeReady() noexcept {
    return true;
}

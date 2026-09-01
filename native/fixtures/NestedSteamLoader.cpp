#include <Windows.h>

#include <iterator>

namespace {

void LoadConfiguredDependency() noexcept {
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(
        L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM",
        path,
        static_cast<DWORD>(std::size(path)));
    if (length != 0 && length < std::size(path)) {
        static_cast<void>(LoadLibraryW(path));
    }
}

}  // namespace

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LoadConfiguredDependency();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl NestedSteamLoaderReady() noexcept {
    return true;
}

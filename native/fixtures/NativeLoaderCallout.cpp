#include <Windows.h>
#include <winternl.h>

#include <cstring>
#include <cwchar>
#include <iterator>
#include <string_view>

extern "C" NTSYSAPI NTSTATUS NTAPI LdrLoadDll(
    PWSTR searchPath,
    PULONG characteristics,
    PUNICODE_STRING name,
    PHANDLE module);
extern "C" NTSYSAPI NTSTATUS NTAPI LdrGetProcedureAddress(
    PVOID module,
    PANSI_STRING name,
    ULONG ordinal,
    PVOID* procedure);

namespace {

void RunConfiguredCallout() noexcept {
    wchar_t target[32768]{};
    wchar_t mode[32]{};
    const auto targetLength = GetEnvironmentVariableW(
        L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_TARGET",
        target,
        static_cast<DWORD>(std::size(target)));
    const auto modeLength = GetEnvironmentVariableW(
        L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_MODE",
        mode,
        static_cast<DWORD>(std::size(mode)));
    if (targetLength == 0 || targetLength >= std::size(target)
        || modeLength == 0 || modeLength >= std::size(mode)) {
        return;
    }
    if (std::wcscmp(mode, L"load") == 0) {
        UNICODE_STRING name{};
        name.Buffer = target;
        name.Length = static_cast<USHORT>(targetLength * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        HANDLE result = nullptr;
        if (LdrLoadDll(nullptr, nullptr, &name, &result) >= 0
            && result != nullptr) {
            ExitProcess(114);
        }
        return;
    }

    const auto slash = std::wstring_view(target, targetLength).find_last_of(
        L"\\/");
    const wchar_t* const baseName = slash == std::wstring_view::npos
        ? target
        : target + slash + 1;
    const HMODULE module = GetModuleHandleW(baseName);
    if (module == nullptr) {
        ExitProcess(116);
    }
    if (std::wcscmp(mode, L"getproc") == 0) {
        if (GetProcAddress(module, "FakeSteamFactory") != nullptr) {
            ExitProcess(112);
        }
        return;
    }
    if (std::wcscmp(mode, L"ldrgetproc") == 0) {
        char procedure[] = "FakeSteamFactory";
        ANSI_STRING name{};
        name.Buffer = procedure;
        name.Length = static_cast<USHORT>(std::strlen(procedure));
        name.MaximumLength = name.Length;
        void* result = nullptr;
        if (LdrGetProcedureAddress(module, &name, 0, &result) >= 0
            && result != nullptr) {
            ExitProcess(113);
        }
    }
}

}  // namespace

BOOL WINAPI DllMain(
    HINSTANCE,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        RunConfiguredCallout();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool __cdecl
NativeLoaderCalloutReady() noexcept {
    return true;
}

#include <Windows.h>

#include <cstdint>

#include "ProtectionBootstrap.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}

extern "C" __declspec(dllexport) std::uint32_t __stdcall InitializeProtection(
    DSRRandomizer::ProtectionInitBlock* block) {
    return static_cast<std::uint32_t>(DSRRandomizer::InitializeProtection(block));
}

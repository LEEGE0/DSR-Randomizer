#include <Windows.h>

#include <cstdint>

#include "ProtectionBootstrap.h"
#include "network/WinsockHooks.h"
#include "save/SaveHooks.h"

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

extern "C" __declspec(dllexport) std::uint32_t __stdcall QuerySaveAuditCounters(
    DSRRandomizer::Save::SaveAuditCounters* counters,
    const std::uint32_t size) {
    if (counters == nullptr || size != sizeof(*counters)) {
        return ERROR_INVALID_PARAMETER;
    }
    *counters = DSRRandomizer::Save::CurrentSaveAuditCounters();
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) std::uint32_t __stdcall QueryWinsockAuditCounters(
    DSRRandomizer::Network::WinsockAuditCounters* counters,
    const std::uint32_t size) {
    if (counters == nullptr || size != sizeof(*counters)) {
        return ERROR_INVALID_PARAMETER;
    }
    *counters = DSRRandomizer::Network::CurrentWinsockAuditCounters();
    return ERROR_SUCCESS;
}

#include <Windows.h>

#include "bridge/RmmBridgeBootstrap.h"
#include "bridge/WindowsBridgePlatform.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
bool modengine_ext_init(void*, void** extension) noexcept {
    if (extension != nullptr) {
        *extension = nullptr;
    }
    DSRRandomizer::Bridge::WindowsBridgePlatform platform;
    const auto result = DSRRandomizer::Bridge::BootstrapRmmBridge(platform);
    if (!result.ok) {
        TerminateProcess(GetCurrentProcess(), result.exitCode);
    }
    return false;
}

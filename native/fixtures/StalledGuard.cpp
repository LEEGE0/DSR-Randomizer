#include <Windows.h>

#include <cstdint>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}

extern "C" __declspec(dllexport) std::uint32_t __stdcall InitializeProtection(void*) {
    Sleep(INFINITE);
    return 0;
}

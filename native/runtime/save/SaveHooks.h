#pragma once

#include <cstdint>
#include <string>

namespace DSRRandomizer::Save {

enum class SaveHookInstallStatus {
    Success,
    InvalidConfiguration,
    InstallFailed,
};

enum class SaveAuditCategory {
    DedicatedRmm,
    DeniedNormal,
    DeniedOverhaul,
    Unrelated,
};

struct SaveAuditCounters {
    std::uint64_t dedicatedRmm;
    std::uint64_t deniedNormal;
    std::uint64_t deniedOverhaul;
    std::uint64_t unrelated;
};

struct SaveHookConfiguration {
    std::wstring virtualDocuments;
    std::wstring virtualLogicalSave;
    std::wstring realSaveRoot;
    std::wstring externalSaveRoot;
    std::wstring dedicatedRmm;
    bool diagnosticMode;
};

class HookPlatform {
public:
    virtual ~HookPlatform() = default;

    virtual bool Initialize() noexcept = 0;
    virtual void* ResolveTarget(
        const wchar_t* moduleName,
        const char* procedureName) noexcept = 0;
    virtual bool CreateHook(
        void* target,
        void* detour,
        void** original) noexcept = 0;
    virtual bool QueueEnable(void* target) noexcept = 0;
    virtual bool ApplyQueued() noexcept = 0;
    virtual void DisableAll() noexcept = 0;
    virtual void RemoveHook(void* target) noexcept = 0;
    virtual void Uninitialize() noexcept = 0;
};

[[nodiscard]] SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration) noexcept;
[[nodiscard]] SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration,
    HookPlatform& platform) noexcept;
void UninstallSaveHooks() noexcept;
[[nodiscard]] bool SaveHooksAreInstalled() noexcept;
[[nodiscard]] SaveAuditCounters CurrentSaveAuditCounters() noexcept;

}  // namespace DSRRandomizer::Save

#pragma once

#include <cstdint>
#include <string>

namespace DSRRandomizer::Save {

enum class SaveHookInstallStatus {
    Success,
    InvalidConfiguration,
    InstallFailed,
};

enum class SaveHookCleanupStatus {
    Success,
    Incomplete,
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

    virtual void BeginMutation() noexcept {}
    virtual void EndMutation() noexcept {}

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
    virtual bool DisableAll() noexcept = 0;
    virtual bool RemoveHook(void* target) noexcept = 0;
    virtual bool Uninitialize() noexcept = 0;
};

[[nodiscard]] SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration) noexcept;
[[nodiscard]] SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration,
    HookPlatform& platform) noexcept;
[[nodiscard]] SaveHookCleanupStatus UninstallSaveHooks() noexcept;
[[nodiscard]] bool SaveHooksAreInstalled() noexcept;
[[nodiscard]] SaveAuditCounters CurrentSaveAuditCounters() noexcept;

namespace Testing {

using BeforeOriginalApiCallback = void (*)(void*) noexcept;

struct SaveHookLifecycleSnapshot {
    bool ready;
    bool contextRetained;
    bool denyOnly;
    std::uint64_t inFlight;
};

[[nodiscard]] SaveHookLifecycleSnapshot CurrentSaveHookLifecycle() noexcept;
void SetBeforeOriginalApiCallback(
    BeforeOriginalApiCallback callback,
    void* state) noexcept;
void HoldSaveHookCallback(void* enteredEvent, void* releaseEvent) noexcept;
void HoldSaveHookCallbackWhileWaitingForMutation(
    void* enteredEvent,
    void* allowMutationEvent,
    void* mutationAcquiredEvent,
    void* releaseEvent) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Save

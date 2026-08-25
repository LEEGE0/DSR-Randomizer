#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "steam/SteamHooks.h"

namespace DSRRandomizer::Modules {

struct DeferredModuleExpectation {
    std::wstring expectedPath;
    std::array<std::uint8_t, 32> expectedSha256{};
    bool allowDeferred = false;
    std::vector<std::string> declaredInterfaces;
    std::vector<std::string> protectedFactoryExports;
};

struct DeferredModuleGateConfiguration {
    std::vector<DeferredModuleExpectation> modules;
    Steam::FatalReporter fatalReporter = nullptr;
    std::shared_ptr<void> identityLease;
};

enum class DeferredModuleGateInstallStatus {
    Success,
    InvalidConfiguration,
    AdmissionFailed,
    HookInstallFailed,
};

enum class DeferredModuleGateCleanupStatus {
    Success,
    Incomplete,
};

[[nodiscard]] DeferredModuleGateInstallStatus InstallDeferredModuleGate(
    const DeferredModuleGateConfiguration& configuration) noexcept;
[[nodiscard]] DeferredModuleGateCleanupStatus UninstallDeferredModuleGate() noexcept;
[[nodiscard]] bool DeferredModuleGateIsInstalled() noexcept;

namespace Testing {

using CountedStringSnapshotHook = void(*)() noexcept;

struct DeferredModuleGateLifecycleSnapshot {
    bool contextRetained;
    bool denyOnly;
    std::size_t hooksRetained;
    std::size_t factorySlotsRetained;
};

struct NativeSymbolDelegationProbeSnapshot {
    std::uint32_t firstOriginalCalls;
    std::uint32_t chainedOriginalCalls;
    std::int32_t status;
    bool resultPublished;
};

// This entry point selects the legacy one-slot interface ABI used only by the
// synthetic Task 3 fixtures. It does not prove launcher suspension; Task 5 owns
// the end-to-end suspended-launch contract.
[[nodiscard]] DeferredModuleGateInstallStatus
InstallDeferredModuleGateForSyntheticSuspendedProcess(
    const DeferredModuleGateConfiguration& configuration) noexcept;
void FailNextFactoryPublication() noexcept;
void FailNextFactoryPostCreateBookkeeping() noexcept;
[[nodiscard]] DeferredModuleGateLifecycleSnapshot
CurrentGateLifecycle() noexcept;
void SetGateCleanupPhaseEvents(
    void* afterInitialDisableEvent,
    void* beforeFactoryDrainEvent) noexcept;
void SetFactoryPublicationPauseEvents(
    void* afterApplyEvent,
    void* allowRollbackEvent) noexcept;
void HoldGateCallbackWhileWaitingForMutation(
    void* enteredEvent,
    void* allowMutationEvent,
    void* mutationAcquiredEvent,
    void* releaseEvent) noexcept;
[[nodiscard]] bool GateIsDenyOnlyForReporter() noexcept;
[[nodiscard]] std::size_t
GateRetainedFactorySlotCountForReporter() noexcept;
[[nodiscard]] std::int32_t CallOriginalLdrLoadDllForSyntheticCallout(
    const std::wstring& path,
    void** module) noexcept;
[[nodiscard]] NativeSymbolDelegationProbeSnapshot
ProbeNativeSymbolDelegationChain() noexcept;
void SetCountedStringSnapshotHook(CountedStringSnapshotHook hook) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Modules

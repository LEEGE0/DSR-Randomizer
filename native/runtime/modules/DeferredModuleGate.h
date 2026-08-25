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

struct DeferredModuleGateLifecycleSnapshot {
    bool contextRetained;
    bool denyOnly;
    std::size_t hooksRetained;
    std::size_t factorySlotsRetained;
};

// Synthetic test subprocesses are started before worker threads and act as the
// Task 5 suspended-launch proof seam. Production callers cannot use the normal
// install API until Task 5 supplies a verifiable proof.
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
void HoldGateCallbackWhileWaitingForMutation(
    void* enteredEvent,
    void* allowMutationEvent,
    void* mutationAcquiredEvent,
    void* releaseEvent) noexcept;
[[nodiscard]] bool GateIsDenyOnlyForReporter() noexcept;
[[nodiscard]] std::size_t
GateRetainedFactorySlotCountForReporter() noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Modules

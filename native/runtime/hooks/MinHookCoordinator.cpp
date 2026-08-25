#include "hooks/MinHookCoordinator.h"

#include <atomic>
#include <exception>
#include <algorithm>
#include <mutex>
#include <vector>

#include <Windows.h>

#include "monitor/HookIntegrityRegistry.h"

namespace DSRRandomizer::Hooks {
namespace {

std::recursive_mutex mutationMutex;
std::size_t referenceCount = 0;
bool ownsInitialization = false;
std::atomic<std::uint32_t> applyCallCount{};
std::atomic<std::uint32_t> failApplyCall{};
std::atomic<std::uint32_t> failDisableCount{};
std::atomic<std::uint32_t> failRemoveCount{};
std::atomic<std::uint32_t> failQueueEnableCount{};
std::atomic<HANDLE> afterLeaseAcquireEvent{};
std::atomic<HANDLE> allowLeaseReleaseEvent{};

struct TrackedHook {
    void* target = nullptr;
    void* trampoline = nullptr;
    bool queued = false;
    bool registered = false;
    std::size_t declaredPatchBytes = Monitor::kDefaultDeclaredPatchBytes;
};

std::vector<TrackedHook> trackedHooks;

auto FindTrackedHook(void* const target) noexcept {
    return std::find_if(
        trackedHooks.begin(),
        trackedHooks.end(),
        [target](const TrackedHook& hook) { return hook.target == target; });
}

bool Consume(std::atomic<std::uint32_t>& remaining) noexcept {
    auto value = remaining.load(std::memory_order_acquire);
    while (value != 0) {
        if (remaining.compare_exchange_weak(
                value,
                value - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

}  // namespace

class MinHookMutationLease::Impl final {
public:
    Impl() noexcept : lock(mutationMutex) {
        const auto afterAcquire = afterLeaseAcquireEvent.load(
            std::memory_order_acquire);
        const auto allowRelease = allowLeaseReleaseEvent.load(
            std::memory_order_acquire);
        if (afterAcquire != nullptr && allowRelease != nullptr) {
            SetEvent(afterAcquire);
            WaitForSingleObject(allowRelease, INFINITE);
        }
    }

private:
    std::unique_lock<std::recursive_mutex> lock;
};

MinHookMutationLease::MinHookMutationLease() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    }
    catch (...) {
        std::terminate();
    }
}

MinHookMutationLease::~MinHookMutationLease() = default;

void MinHookMutationLease::Release() noexcept {
    impl_.reset();
}

bool AcquireMinHook() noexcept {
    std::scoped_lock lock(mutationMutex);
    if (referenceCount != 0) {
        ++referenceCount;
        return true;
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }
    ownsInitialization = status == MH_OK;
    referenceCount = 1;
    return true;
}

bool ReleaseMinHook() noexcept {
    std::scoped_lock lock(mutationMutex);
    if (referenceCount == 0) {
        return true;
    }
    if (referenceCount > 1) {
        --referenceCount;
        return true;
    }
    if (ownsInitialization) {
        const auto status = MH_Uninitialize();
        if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED) {
            return false;
        }
    }
    referenceCount = 0;
    ownsInitialization = false;
    trackedHooks.clear();
    Monitor::ClearHookIntegrityRegistry();
    return true;
}

MH_STATUS CreateHook(
    void* const target,
    void* const detour,
    void** const original) noexcept {
    std::scoped_lock lock(mutationMutex);
    void* capturedTrampoline = nullptr;
    auto* const trampolineDestination = original == nullptr
        ? &capturedTrampoline
        : original;
    const auto status = MH_CreateHook(target, detour, trampolineDestination);
    if (status != MH_OK) {
        return status;
    }
    capturedTrampoline = *trampolineDestination;
    try {
        trackedHooks.push_back({
            target,
            capturedTrampoline,
            false,
            false,
            Monitor::kDefaultDeclaredPatchBytes});
    }
    catch (...) {
        static_cast<void>(MH_RemoveHook(target));
        return MH_ERROR_MEMORY_ALLOC;
    }
    return MH_OK;
}

bool SetDeclaredPatchBytes(
    void* const target,
    const std::size_t declaredPatchBytes) noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto found = FindTrackedHook(target);
    if (found == trackedHooks.end() || declaredPatchBytes == 0
        || declaredPatchBytes > Monitor::kMaximumDeclaredPatchBytes
        || found->registered) {
        return false;
    }
    found->declaredPatchBytes = declaredPatchBytes;
    return true;
}

MH_STATUS QueueEnableHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto status = Consume(failQueueEnableCount)
        ? MH_ERROR_MEMORY_ALLOC
        : MH_QueueEnableHook(target);
    if (status == MH_OK) {
        const auto found = FindTrackedHook(target);
        if (found != trackedHooks.end()) {
            found->queued = true;
        }
    }
    return status;
}

MH_STATUS ApplyQueuedHooks() noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto call = applyCallCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto failAt = failApplyCall.load(std::memory_order_acquire);
    const auto status = failAt != 0 && call == failAt
        ? MH_ERROR_MEMORY_ALLOC
        : MH_ApplyQueued();
    if (status != MH_OK) {
        return status;
    }
    for (auto& hook : trackedHooks) {
        if (!hook.queued) {
            continue;
        }
        hook.queued = false;
        if (!Monitor::RegisterInstalledHook(
                hook.target,
                hook.trampoline,
                hook.declaredPatchBytes)) {
            return MH_ERROR_MEMORY_ALLOC;
        }
        hook.registered = true;
    }
    return MH_OK;
}

MH_STATUS DisableHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto status = Consume(failDisableCount)
        ? MH_ERROR_MEMORY_PROTECT
        : MH_DisableHook(target);
    if (status == MH_OK || status == MH_ERROR_DISABLED
        || status == MH_ERROR_NOT_CREATED) {
        Monitor::UnregisterInstalledHook(target);
        const auto found = FindTrackedHook(target);
        if (found != trackedHooks.end()) {
            found->registered = false;
            found->queued = false;
        }
    }
    return status;
}

MH_STATUS RemoveHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto status = Consume(failRemoveCount)
        ? MH_ERROR_MEMORY_PROTECT
        : MH_RemoveHook(target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
        Monitor::UnregisterInstalledHook(target);
        const auto found = FindTrackedHook(target);
        if (found != trackedHooks.end()) {
            trackedHooks.erase(found);
        }
    }
    return status;
}

namespace Testing {

void SetMinHookFaults(const MinHookFaults& faults) noexcept {
    std::scoped_lock lock(mutationMutex);
    applyCallCount.store(0, std::memory_order_release);
    failApplyCall.store(faults.failApplyCall, std::memory_order_release);
    failDisableCount.store(faults.failDisableCount, std::memory_order_release);
    failRemoveCount.store(faults.failRemoveCount, std::memory_order_release);
    failQueueEnableCount.store(
        faults.failQueueEnableCount,
        std::memory_order_release);
}

std::size_t MinHookReferenceCount() noexcept {
    std::scoped_lock lock(mutationMutex);
    return referenceCount;
}

void SetMutationLeasePauseEvents(
    void* const afterAcquire,
    void* const allowRelease) noexcept {
    afterLeaseAcquireEvent.store(
        static_cast<HANDLE>(afterAcquire), std::memory_order_release);
    allowLeaseReleaseEvent.store(
        static_cast<HANDLE>(allowRelease), std::memory_order_release);
}

}  // namespace Testing

}  // namespace DSRRandomizer::Hooks

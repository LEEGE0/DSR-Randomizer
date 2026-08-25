#include "hooks/MinHookCoordinator.h"

#include <atomic>
#include <exception>
#include <mutex>

namespace DSRRandomizer::Hooks {
namespace {

std::recursive_mutex mutationMutex;
std::size_t referenceCount = 0;
bool ownsInitialization = false;
std::atomic<std::uint32_t> applyCallCount{};
std::atomic<std::uint32_t> failApplyCall{};
std::atomic<std::uint32_t> failDisableCount{};
std::atomic<std::uint32_t> failRemoveCount{};

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
    Impl() noexcept : lock(mutationMutex) {}

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
    return true;
}

MH_STATUS CreateHook(
    void* const target,
    void* const detour,
    void** const original) noexcept {
    std::scoped_lock lock(mutationMutex);
    return MH_CreateHook(target, detour, original);
}

MH_STATUS QueueEnableHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    return MH_QueueEnableHook(target);
}

MH_STATUS ApplyQueuedHooks() noexcept {
    std::scoped_lock lock(mutationMutex);
    const auto call = applyCallCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto failAt = failApplyCall.load(std::memory_order_acquire);
    return failAt != 0 && call == failAt
        ? MH_ERROR_MEMORY_ALLOC
        : MH_ApplyQueued();
}

MH_STATUS DisableHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    return Consume(failDisableCount)
        ? MH_ERROR_MEMORY_PROTECT
        : MH_DisableHook(target);
}

MH_STATUS RemoveHook(void* const target) noexcept {
    std::scoped_lock lock(mutationMutex);
    return Consume(failRemoveCount)
        ? MH_ERROR_MEMORY_PROTECT
        : MH_RemoveHook(target);
}

namespace Testing {

void SetMinHookFaults(const MinHookFaults& faults) noexcept {
    std::scoped_lock lock(mutationMutex);
    applyCallCount.store(0, std::memory_order_release);
    failApplyCall.store(faults.failApplyCall, std::memory_order_release);
    failDisableCount.store(faults.failDisableCount, std::memory_order_release);
    failRemoveCount.store(faults.failRemoveCount, std::memory_order_release);
}

std::size_t MinHookReferenceCount() noexcept {
    std::scoped_lock lock(mutationMutex);
    return referenceCount;
}

}  // namespace Testing

}  // namespace DSRRandomizer::Hooks

#pragma once

#include <MinHook.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace DSRRandomizer::Hooks {

class MinHookMutationLease final {
public:
    MinHookMutationLease() noexcept;
    ~MinHookMutationLease();
    MinHookMutationLease(const MinHookMutationLease&) = delete;
    MinHookMutationLease& operator=(const MinHookMutationLease&) = delete;
    void Release() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool AcquireMinHook() noexcept;
[[nodiscard]] bool ReleaseMinHook() noexcept;
[[nodiscard]] MH_STATUS CreateHook(
    void* target,
    void* detour,
    void** original) noexcept;
[[nodiscard]] MH_STATUS QueueEnableHook(void* target) noexcept;
[[nodiscard]] MH_STATUS ApplyQueuedHooks() noexcept;
[[nodiscard]] MH_STATUS DisableHook(void* target) noexcept;
[[nodiscard]] MH_STATUS RemoveHook(void* target) noexcept;

namespace Testing {

struct MinHookFaults {
    std::uint32_t failApplyCall = 0;
    std::uint32_t failDisableCount = 0;
    std::uint32_t failRemoveCount = 0;
    std::uint32_t failQueueEnableCount = 0;
};

void SetMinHookFaults(const MinHookFaults& faults) noexcept;
[[nodiscard]] std::size_t MinHookReferenceCount() noexcept;
void SetMutationLeasePauseEvents(
    void* afterAcquireEvent,
    void* allowReleaseEvent) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Hooks

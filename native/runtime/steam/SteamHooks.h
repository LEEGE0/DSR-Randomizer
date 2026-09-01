#pragma once

#include <cstddef>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DSRRandomizer::Steam {

using FatalReporter = void(*)(const char* code) noexcept;

namespace Synthetic {

struct Interface;
using InvokeFunction = bool(*)(void* self) noexcept;

struct InterfaceVTable {
    InvokeFunction Invoke;
};

struct Interface {
    const InterfaceVTable* vtable;
};

using FactoryFunction = void*(__cdecl*)(const char* version) noexcept;

}  // namespace Synthetic

inline constexpr std::size_t kSteamFactorySlotCapacity = 8;

class FatalState final {
public:
    explicit FatalState(FatalReporter reporter) noexcept;

    void EnterDenyOnly() noexcept;
    void Trigger(const char* code) noexcept;
    [[nodiscard]] bool IsFatal() const noexcept;

private:
    FatalReporter reporter_;
    std::atomic<bool> fatal_{false};
};

enum class SteamFactorySlotStatus {
    Success,
    InvalidConfiguration,
    SlotUnavailable,
};

enum class SteamInterfaceLayout {
    ProductionPinned,
    SyntheticOneSlotForTesting,
};

class SteamFactoryCallbackBlock final {
public:
    SteamFactoryCallbackBlock() noexcept;
    ~SteamFactoryCallbackBlock();
    SteamFactoryCallbackBlock(const SteamFactoryCallbackBlock&) = delete;
    SteamFactoryCallbackBlock& operator=(const SteamFactoryCallbackBlock&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] SteamFactorySlotStatus RegisterSteamFactorySlot(
    std::size_t slot,
    const std::vector<std::string>& declaredInterfaces,
    const std::shared_ptr<FatalState>& fatalState,
    SteamInterfaceLayout layout) noexcept;
[[nodiscard]] bool SetSteamFactoryOriginal(
    std::size_t slot,
    Synthetic::FactoryFunction original) noexcept;
[[nodiscard]] void* SteamFactoryDetourAddress(std::size_t slot) noexcept;
void UnregisterSteamFactorySlot(std::size_t slot) noexcept;

namespace Testing {

[[nodiscard]] std::size_t ProductionInterfaceSlotCount(
    std::string_view version) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Steam

#pragma once

#include <cstddef>
#include <memory>
#include <string>
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

enum class SteamFactorySlotStatus {
    Success,
    InvalidConfiguration,
    SlotUnavailable,
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
    FatalReporter fatalReporter) noexcept;
[[nodiscard]] bool SetSteamFactoryOriginal(
    std::size_t slot,
    Synthetic::FactoryFunction original) noexcept;
[[nodiscard]] void* SteamFactoryDetourAddress(std::size_t slot) noexcept;
void UnregisterSteamFactorySlot(std::size_t slot) noexcept;

}  // namespace DSRRandomizer::Steam

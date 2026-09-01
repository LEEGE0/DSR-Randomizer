#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DSRRandomizer::Game {

enum class InternalTargetAction : std::uint8_t {
    ForceOffline = 1,
    DenyCall = 2,
};

struct GameServiceImage {
    std::wstring moduleName;
    const std::byte* base = nullptr;
    std::size_t size = 0;
};

struct GameServiceTarget {
    std::wstring moduleName;
    std::uintptr_t rva = 0;
    std::array<std::uint8_t, 32> fingerprintSha256{};
    std::size_t patchLength = 0;
    InternalTargetAction action = InternalTargetAction::DenyCall;
};

struct GameServiceGuardConfiguration {
    std::vector<GameServiceImage> images;
    std::vector<GameServiceTarget> targets;
    std::shared_ptr<void> identityLease;
};

enum class GameServiceGuardInstallStatus {
    Success,
    InvalidConfiguration,
    ProfileMismatch,
    HookFailed,
};

enum class GameServiceGuardCleanupStatus {
    Success,
    Incomplete,
};

struct GameServiceGuardLifecycleSnapshot {
    bool installed = false;
    bool cleanupIncomplete = false;
    bool denyOnlyRetained = false;
    std::size_t installedHookCount = 0;
};

struct HookBackend {
    bool (*acquire)() noexcept = nullptr;
    bool (*release)() noexcept = nullptr;
    bool (*create)(void*, void*, void**) noexcept = nullptr;
    bool (*queueEnable)(void*) noexcept = nullptr;
    bool (*applyQueued)() noexcept = nullptr;
    bool (*disable)(void*) noexcept = nullptr;
    bool (*remove)(void*) noexcept = nullptr;
};

[[nodiscard]] GameServiceGuardInstallStatus InstallGameServiceGuard(
    const GameServiceGuardConfiguration& configuration) noexcept;
[[nodiscard]] GameServiceGuardInstallStatus
InstallPinnedGameServiceGuard() noexcept;
[[nodiscard]] GameServiceGuardCleanupStatus UninstallGameServiceGuard() noexcept;
[[nodiscard]] GameServiceGuardLifecycleSnapshot
CurrentGameServiceGuardLifecycle() noexcept;

[[nodiscard]] GameServiceGuardInstallStatus
InstallGameServiceGuardForTesting(
    const GameServiceGuardConfiguration& configuration,
    const HookBackend& backend) noexcept;

}  // namespace DSRRandomizer::Game

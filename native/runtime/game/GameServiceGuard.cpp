#include "game/GameServiceGuard.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "hooks/MinHookCoordinator.h"
#include "profile/PinnedCompatibilityProfile.h"

namespace DSRRandomizer::Game {
namespace {

using OfflineSetter = void(*)(bool) noexcept;

struct HookEntry {
    void* address = nullptr;
    InternalTargetAction action = InternalTargetAction::DenyCall;
};

struct GuardState {
    HookBackend backend{};
    std::vector<HookEntry> hooks;
    std::shared_ptr<void> identityLease;
    bool acquired = false;
    bool installed = false;
    bool cleanupIncomplete = false;
};

struct ResolvedTarget {
    void* address = nullptr;
    InternalTargetAction action{};
};

std::mutex guardMutex;
GuardState guardState;
std::atomic<OfflineSetter> offlineSetter{};

void ForceOfflineDetour(bool) noexcept {
    const auto setter = offlineSetter.load(std::memory_order_acquire);
    if (setter != nullptr) {
        setter(false);
    }
}

std::uintptr_t DenyCallDetour() noexcept {
    return 0;
}

bool MinHookAcquire() noexcept {
    return Hooks::AcquireMinHook();
}

bool MinHookRelease() noexcept {
    return Hooks::ReleaseMinHook();
}

bool MinHookCreate(void* target, void* detour, void** original) noexcept {
    return Hooks::CreateHook(target, detour, original) == MH_OK;
}

bool MinHookQueue(void* target) noexcept {
    return Hooks::QueueEnableHook(target) == MH_OK;
}

bool MinHookApply() noexcept {
    return Hooks::ApplyQueuedHooks() == MH_OK;
}

bool MinHookDisable(void* target) noexcept {
    const auto status = Hooks::DisableHook(target);
    return status == MH_OK || status == MH_ERROR_DISABLED;
}

bool MinHookRemove(void* target) noexcept {
    return Hooks::RemoveHook(target) == MH_OK;
}

HookBackend ProductionBackend() noexcept {
    return {
        &MinHookAcquire,
        &MinHookRelease,
        &MinHookCreate,
        &MinHookQueue,
        &MinHookApply,
        &MinHookDisable,
        &MinHookRemove,
    };
}

bool ValidBackend(const HookBackend& backend) noexcept {
    return backend.acquire != nullptr
        && backend.release != nullptr
        && backend.create != nullptr
        && backend.queueEnable != nullptr
        && backend.applyQueued != nullptr
        && backend.disable != nullptr
        && backend.remove != nullptr;
}

bool EqualModuleName(
    const std::wstring& left,
    const std::wstring& right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
            left.data(),
            static_cast<int>(left.size()),
            right.data(),
            static_cast<int>(right.size()),
            TRUE)
        == CSTR_EQUAL;
}

bool HashWindow(
    const std::byte* bytes,
    const std::size_t length,
    std::array<std::uint8_t, 32>& result) noexcept {
    if (bytes == nullptr || length == 0
        || length > static_cast<std::size_t>(std::numeric_limits<ULONG>::max())) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) != 0) {
        return false;
    }
    const auto status = BCryptHash(
        algorithm,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes)),
        static_cast<ULONG>(length),
        result.data(),
        static_cast<ULONG>(result.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

GameServiceGuardInstallStatus ResolveAndVerify(
    const GameServiceGuardConfiguration& configuration,
    std::vector<ResolvedTarget>& resolved) noexcept {
    if (configuration.images.empty() || configuration.targets.empty()) {
        return GameServiceGuardInstallStatus::InvalidConfiguration;
    }

    bool hasForceOffline = false;
    bool hasDenyCall = false;
    try {
        resolved.reserve(configuration.targets.size());
        for (const auto& target : configuration.targets) {
            if (target.moduleName.empty() || target.rva == 0
                || target.patchLength == 0) {
                return GameServiceGuardInstallStatus::InvalidConfiguration;
            }

            const auto image = std::find_if(
                configuration.images.begin(),
                configuration.images.end(),
                [&target](const GameServiceImage& candidate) {
                    return EqualModuleName(candidate.moduleName, target.moduleName);
                });
            if (image == configuration.images.end() || image->base == nullptr
                || target.rva > image->size
                || target.patchLength > image->size - target.rva) {
                return GameServiceGuardInstallStatus::ProfileMismatch;
            }

            auto* const address = const_cast<std::byte*>(image->base + target.rva);
            if (std::any_of(
                    resolved.begin(),
                    resolved.end(),
                    [address](const ResolvedTarget& candidate) {
                        return candidate.address == address;
                    })) {
                return GameServiceGuardInstallStatus::InvalidConfiguration;
            }

            std::array<std::uint8_t, 32> fingerprint{};
            if (!HashWindow(address, target.patchLength, fingerprint)
                || fingerprint != target.fingerprintSha256) {
                return GameServiceGuardInstallStatus::ProfileMismatch;
            }

            switch (target.action) {
            case InternalTargetAction::ForceOffline:
                hasForceOffline = true;
                break;
            case InternalTargetAction::DenyCall:
                hasDenyCall = true;
                break;
            default:
                return GameServiceGuardInstallStatus::InvalidConfiguration;
            }
            resolved.push_back({address, target.action});
        }
    }
    catch (...) {
        return GameServiceGuardInstallStatus::InvalidConfiguration;
    }

    return hasForceOffline && hasDenyCall
        ? GameServiceGuardInstallStatus::Success
        : GameServiceGuardInstallStatus::InvalidConfiguration;
}

bool CleanupLocked() noexcept {
    bool complete = true;
    std::vector<HookEntry> disabledHooks;
    std::vector<HookEntry> remaining;
    try {
        disabledHooks.reserve(guardState.hooks.size());
        remaining.reserve(guardState.hooks.size());
    }
    catch (...) {
        guardState.cleanupIncomplete = true;
        return false;
    }

    for (auto current = guardState.hooks.rbegin();
         current != guardState.hooks.rend();
         ++current) {
        const bool disabled = guardState.backend.disable(current->address);
        if (!disabled) {
            complete = false;
            remaining.push_back(*current);
        }
        else {
            disabledHooks.push_back(*current);
        }
    }
    for (const auto& current : disabledHooks) {
        const bool removed = guardState.backend.remove(current.address);
        if (!removed) {
            complete = false;
            remaining.push_back(current);
        }
    }
    std::reverse(remaining.begin(), remaining.end());
    guardState.hooks = std::move(remaining);
    guardState.installed = false;

    if (std::none_of(
            guardState.hooks.begin(),
            guardState.hooks.end(),
            [](const HookEntry& hook) {
                return hook.action == InternalTargetAction::ForceOffline;
            })) {
        offlineSetter.store(nullptr, std::memory_order_release);
    }

    if (guardState.hooks.empty() && guardState.acquired) {
        if (guardState.backend.release()) {
            guardState.acquired = false;
        }
        else {
            complete = false;
        }
    }
    guardState.cleanupIncomplete = !complete;
    if (complete) {
        guardState = {};
    }
    return complete;
}

GameServiceGuardInstallStatus InstallWithBackend(
    const GameServiceGuardConfiguration& configuration,
    const HookBackend& backend) noexcept {
    std::scoped_lock lock(guardMutex);
    if (!ValidBackend(backend) || guardState.acquired || !guardState.hooks.empty()) {
        return GameServiceGuardInstallStatus::InvalidConfiguration;
    }

    std::vector<ResolvedTarget> resolved;
    const auto verification = ResolveAndVerify(configuration, resolved);
    if (verification != GameServiceGuardInstallStatus::Success) {
        return verification;
    }

    guardState.backend = backend;
    guardState.identityLease = configuration.identityLease;
    if (!backend.acquire()) {
        guardState = {};
        return GameServiceGuardInstallStatus::HookFailed;
    }
    guardState.acquired = true;

    try {
        guardState.hooks.reserve(resolved.size());
        for (const auto& target : resolved) {
            void* const detour = target.action == InternalTargetAction::ForceOffline
                ? reinterpret_cast<void*>(&ForceOfflineDetour)
                : reinterpret_cast<void*>(&DenyCallDetour);
            void* original = nullptr;
            if (!backend.create(
                    target.address,
                    detour,
                    target.action == InternalTargetAction::ForceOffline
                        ? &original
                        : nullptr)
                || (target.action == InternalTargetAction::ForceOffline
                    && original == nullptr)) {
                static_cast<void>(CleanupLocked());
                return GameServiceGuardInstallStatus::HookFailed;
            }
            if (target.action == InternalTargetAction::ForceOffline) {
                offlineSetter.store(
                    reinterpret_cast<OfflineSetter>(original),
                    std::memory_order_release);
            }
            guardState.hooks.push_back({target.address, target.action});
        }
        for (const auto& target : resolved) {
            if (!backend.queueEnable(target.address)) {
                static_cast<void>(CleanupLocked());
                return GameServiceGuardInstallStatus::HookFailed;
            }
        }
        if (!backend.applyQueued()) {
            static_cast<void>(CleanupLocked());
            return GameServiceGuardInstallStatus::HookFailed;
        }
        const auto setter = offlineSetter.load(std::memory_order_acquire);
        if (setter == nullptr) {
            static_cast<void>(CleanupLocked());
            return GameServiceGuardInstallStatus::HookFailed;
        }
        setter(false);
    }
    catch (...) {
        static_cast<void>(CleanupLocked());
        return GameServiceGuardInstallStatus::HookFailed;
    }

    guardState.installed = true;
    guardState.cleanupIncomplete = false;
    return GameServiceGuardInstallStatus::Success;
}

}  // namespace

GameServiceGuardInstallStatus InstallGameServiceGuard(
    const GameServiceGuardConfiguration& configuration) noexcept {
    Hooks::MinHookMutationLease mutation;
    return InstallWithBackend(configuration, ProductionBackend());
}

GameServiceGuardInstallStatus InstallPinnedGameServiceGuard() noexcept {
    Profile::PinnedCompatibilityProfile profile{};
    const auto status = Profile::BuildPinnedCompatibilityProfile(profile);
    if (status != Profile::PinnedCompatibilityProfileStatus::Success) {
        return GameServiceGuardInstallStatus::ProfileMismatch;
    }
    return InstallGameServiceGuard(profile.gameService);
}

GameServiceGuardInstallStatus InstallGameServiceGuardForTesting(
    const GameServiceGuardConfiguration& configuration,
    const HookBackend& backend) noexcept {
    Hooks::MinHookMutationLease mutation;
    return InstallWithBackend(configuration, backend);
}

GameServiceGuardCleanupStatus UninstallGameServiceGuard() noexcept {
    Hooks::MinHookMutationLease mutation;
    std::scoped_lock lock(guardMutex);
    return CleanupLocked()
        ? GameServiceGuardCleanupStatus::Success
        : GameServiceGuardCleanupStatus::Incomplete;
}

GameServiceGuardLifecycleSnapshot CurrentGameServiceGuardLifecycle() noexcept {
    std::scoped_lock lock(guardMutex);
    return {
        guardState.installed,
        guardState.cleanupIncomplete,
        guardState.cleanupIncomplete && !guardState.hooks.empty(),
        guardState.hooks.size(),
    };
}

}  // namespace DSRRandomizer::Game

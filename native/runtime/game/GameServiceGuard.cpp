#include "game/GameServiceGuard.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "hooks/MinHookCoordinator.h"
#include "game/CompatibilityProfile.generated.h"

namespace DSRRandomizer::Game {
namespace {

struct GuardState {
    HookBackend backend{};
    std::vector<void*> hooks;
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

bool ForceOfflineDetour() noexcept {
    return false;
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
    std::vector<void*> remaining;
    try {
        remaining.reserve(guardState.hooks.size());
    }
    catch (...) {
        guardState.cleanupIncomplete = true;
        return false;
    }

    for (auto current = guardState.hooks.rbegin();
         current != guardState.hooks.rend();
         ++current) {
        const bool disabled = guardState.backend.disable(*current);
        const bool removed = guardState.backend.remove(*current);
        if (!disabled) {
            complete = false;
        }
        if (!removed) {
            complete = false;
            remaining.push_back(*current);
        }
    }
    std::reverse(remaining.begin(), remaining.end());
    guardState.hooks = std::move(remaining);
    guardState.installed = false;

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
            if (!backend.create(target.address, detour, nullptr)) {
                static_cast<void>(CleanupLocked());
                return GameServiceGuardInstallStatus::HookFailed;
            }
            guardState.hooks.push_back(target.address);
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
    return InstallWithBackend(configuration, ProductionBackend());
}

GameServiceGuardInstallStatus InstallPinnedGameServiceGuard() noexcept {
    const auto module = GetModuleHandleW(Generated::kExecutableModule);
    if (module == nullptr) {
        return GameServiceGuardInstallStatus::ProfileMismatch;
    }
    const auto* const base = reinterpret_cast<const std::byte*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return GameServiceGuardInstallStatus::ProfileMismatch;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != Generated::kMachine
        || nt->FileHeader.TimeDateStamp != Generated::kTimestamp
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.SizeOfImage != Generated::kSizeOfImage) {
        return GameServiceGuardInstallStatus::ProfileMismatch;
    }

    try {
        GameServiceGuardConfiguration configuration{};
        configuration.images.push_back({
            Generated::kExecutableModule,
            base,
            nt->OptionalHeader.SizeOfImage,
        });
        configuration.targets.reserve(Generated::kTargets.size());
        for (const auto& target : Generated::kTargets) {
            configuration.targets.push_back({
                target.moduleName,
                target.rva,
                target.fingerprintSha256,
                target.patchLength,
                target.action,
            });
        }
        return InstallGameServiceGuard(configuration);
    }
    catch (...) {
        return GameServiceGuardInstallStatus::InvalidConfiguration;
    }
}

GameServiceGuardInstallStatus InstallGameServiceGuardForTesting(
    const GameServiceGuardConfiguration& configuration,
    const HookBackend& backend) noexcept {
    return InstallWithBackend(configuration, backend);
}

GameServiceGuardCleanupStatus UninstallGameServiceGuard() noexcept {
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
        guardState.hooks.size(),
    };
}

}  // namespace DSRRandomizer::Game

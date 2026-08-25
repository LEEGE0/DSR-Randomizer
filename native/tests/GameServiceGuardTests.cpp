#include "game/GameServiceGuard.h"
#include "ProtectionBootstrap.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace DSRRandomizer::Game;

static_assert(
    static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::GameServiceOffline)
    == (1ULL << 6));

struct FakeHooks {
    static inline bool acquired = false;
    static inline std::size_t createCalls = 0;
    static inline std::size_t queueCalls = 0;
    static inline std::size_t applyCalls = 0;
    static inline std::size_t installed = 0;
    static inline std::size_t failCreateCall = 0;
    static inline bool failApply = false;
    static inline std::vector<void*> detours;

    static void Reset() noexcept {
        acquired = false;
        createCalls = 0;
        queueCalls = 0;
        applyCalls = 0;
        installed = 0;
        failCreateCall = 0;
        failApply = false;
        detours.clear();
    }

    static bool Acquire() noexcept {
        acquired = true;
        return true;
    }
    static bool Release() noexcept {
        acquired = false;
        return true;
    }
    static bool Create(void*, void* detour, void**) noexcept {
        ++createCalls;
        if (failCreateCall != 0 && createCalls == failCreateCall) {
            return false;
        }
        detours.push_back(detour);
        ++installed;
        return true;
    }
    static bool Queue(void*) noexcept {
        ++queueCalls;
        return true;
    }
    static bool Apply() noexcept {
        ++applyCalls;
        return !failApply;
    }
    static bool Disable(void*) noexcept { return true; }
    static bool Remove(void*) noexcept {
        if (installed != 0) {
            --installed;
        }
        return true;
    }
};

HookBackend Backend() noexcept {
    return {
        &FakeHooks::Acquire,
        &FakeHooks::Release,
        &FakeHooks::Create,
        &FakeHooks::Queue,
        &FakeHooks::Apply,
        &FakeHooks::Disable,
        &FakeHooks::Remove,
    };
}

std::array<std::uint8_t, 32> Hash(std::span<const std::byte> bytes) {
    std::array<std::uint8_t, 32> result{};
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        std::terminate();
    }
    const auto status = BCryptHash(
        algorithm,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),
        result.data(),
        static_cast<ULONG>(result.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        std::terminate();
    }
    return result;
}

GameServiceGuardConfiguration Configuration(
    std::array<std::byte, 128>& image) {
    constexpr std::size_t forceOffset = 16;
    constexpr std::size_t denyOffset = 48;
    constexpr std::size_t patchLength = 16;
    return {
        {GameServiceImage{L"fixture.exe", image.data(), image.size()}},
        {
            GameServiceTarget{
                L"fixture.exe",
                forceOffset,
                Hash(std::span(image).subspan(forceOffset, patchLength)),
                patchLength,
                InternalTargetAction::ForceOffline},
            GameServiceTarget{
                L"fixture.exe",
                denyOffset,
                Hash(std::span(image).subspan(denyOffset, patchLength)),
                patchLength,
                InternalTargetAction::DenyCall},
        },
    };
}

bool RejectsFingerprintMismatchBeforeHookMutation() {
    FakeHooks::Reset();
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);
    image[48] = std::byte{1};

    return InstallGameServiceGuardForTesting(configuration, Backend())
            == GameServiceGuardInstallStatus::ProfileMismatch
        && !FakeHooks::acquired
        && FakeHooks::createCalls == 0
        && FakeHooks::installed == 0
        && CurrentGameServiceGuardLifecycle().installedHookCount == 0;
}

bool InstallsOneAtomicGroupAndUninstallsCleanly() {
    FakeHooks::Reset();
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);

    const auto install =
        InstallGameServiceGuardForTesting(configuration, Backend());
    const auto active = CurrentGameServiceGuardLifecycle();
    const auto forceOffline =
        reinterpret_cast<bool(*)() noexcept>(FakeHooks::detours.at(0));
    const auto denyCall =
        reinterpret_cast<std::uintptr_t(*)() noexcept>(FakeHooks::detours.at(1));
    const bool detoursDeny = !forceOffline() && denyCall() == 0;
    const auto cleanup = UninstallGameServiceGuard();
    return install == GameServiceGuardInstallStatus::Success
        && active.installed
        && active.installedHookCount == 2
        && FakeHooks::createCalls == 2
        && FakeHooks::queueCalls == 2
        && FakeHooks::applyCalls == 1
        && detoursDeny
        && cleanup == GameServiceGuardCleanupStatus::Success
        && FakeHooks::installed == 0
        && !FakeHooks::acquired;
}

bool ApplyFailureRollsBackEveryCreatedHook() {
    FakeHooks::Reset();
    FakeHooks::failApply = true;
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);

    return InstallGameServiceGuardForTesting(configuration, Backend())
            == GameServiceGuardInstallStatus::HookFailed
        && FakeHooks::createCalls == 2
        && FakeHooks::queueCalls == 2
        && FakeHooks::applyCalls == 1
        && FakeHooks::installed == 0
        && !FakeHooks::acquired
        && CurrentGameServiceGuardLifecycle().installedHookCount == 0;
}

bool CreateFailureRollsBackEarlierHooks() {
    FakeHooks::Reset();
    FakeHooks::failCreateCall = 2;
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);

    return InstallGameServiceGuardForTesting(configuration, Backend())
            == GameServiceGuardInstallStatus::HookFailed
        && FakeHooks::createCalls == 2
        && FakeHooks::installed == 0
        && !FakeHooks::acquired
        && CurrentGameServiceGuardLifecycle().installedHookCount == 0;
}

bool BootstrapFailsClosedWhenPinnedImageDoesNotMatch() {
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = sizeof(block);
    block.requiredFlags =
        static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::GameServiceOffline);

    const auto status = DSRRandomizer::InitializeForTest(&block);
    return status == DSRRandomizer::InitStatus::GameServiceProfileMismatch
        && DSRRandomizer::CurrentProtectionFlags()
            == DSRRandomizer::ProtectionFlags::None
        && CurrentGameServiceGuardLifecycle().installedHookCount == 0;
}

}  // namespace

int wmain() {
    const std::array tests{
        std::pair{"fingerprint mismatch is pre-mutation",
                  &RejectsFingerprintMismatchBeforeHookMutation},
        std::pair{"atomic group installs and cleans up",
                  &InstallsOneAtomicGroupAndUninstallsCleanly},
        std::pair{"apply failure rolls back", &ApplyFailureRollsBackEveryCreatedHook},
        std::pair{"create failure rolls back", &CreateFailureRollsBackEarlierHooks},
        std::pair{"bootstrap rejects an unprofiled image",
                  &BootstrapFailsClosedWhenPinnedImageDoesNotMatch},
    };

    for (const auto& [name, test] : tests) {
        if (!test()) {
            std::wcerr << L"FAIL: " << name << L'\n';
            return 1;
        }
    }
    return 0;
}

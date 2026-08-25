#include "game/GameServiceGuard.h"
#include "ProtectionBootstrap.h"
#include "hooks/MinHookCoordinator.h"
#include "network/WinsockHooks.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace DSRRandomizer::Game;

volatile bool productionOfflineState = true;
volatile std::uint32_t productionOfflineCalls = 0;

__declspec(noinline) void ProductionOfflineSetter(const bool value) noexcept {
    productionOfflineState = value;
    ++productionOfflineCalls;
}

__declspec(noinline) std::uintptr_t ProductionDeniedCall() noexcept {
    return productionOfflineCalls + 99;
}

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
    static inline std::size_t disableCalls = 0;
    static inline std::size_t removeCalls = 0;
    static inline std::size_t failDisableCall = 0;
    static inline std::size_t failRemoveCall = 0;
    static inline bool failRelease = false;
    static inline std::size_t offlineSetterCalls = 0;
    static inline bool lastOfflineSetterValue = true;
    static inline std::vector<void*> detours;

    static void Reset() noexcept {
        acquired = false;
        createCalls = 0;
        queueCalls = 0;
        applyCalls = 0;
        installed = 0;
        failCreateCall = 0;
        failApply = false;
        disableCalls = 0;
        removeCalls = 0;
        failDisableCall = 0;
        failRemoveCall = 0;
        failRelease = false;
        offlineSetterCalls = 0;
        lastOfflineSetterValue = true;
        detours.clear();
    }

    static bool Acquire() noexcept {
        acquired = true;
        return true;
    }
    static bool Release() noexcept {
        if (failRelease) {
            return false;
        }
        acquired = false;
        return true;
    }
    static void OfflineSetter(bool value) noexcept {
        ++offlineSetterCalls;
        lastOfflineSetterValue = value;
    }
    static bool Create(void*, void* detour, void** original) noexcept {
        ++createCalls;
        if (failCreateCall != 0 && createCalls == failCreateCall) {
            return false;
        }
        detours.push_back(detour);
        if (createCalls == 1 && original != nullptr) {
            *original = reinterpret_cast<void*>(&OfflineSetter);
        }
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
    static bool Disable(void*) noexcept {
        ++disableCalls;
        return failDisableCall == 0 || disableCalls != failDisableCall;
    }
    static bool Remove(void*) noexcept {
        ++removeCalls;
        if (failRemoveCall != 0 && removeCalls == failRemoveCall) {
            return false;
        }
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
        reinterpret_cast<void(*)(bool) noexcept>(FakeHooks::detours.at(0));
    const auto denyCall =
        reinterpret_cast<std::uintptr_t(*)() noexcept>(FakeHooks::detours.at(1));
    const auto callsAfterInstall = FakeHooks::offlineSetterCalls;
    forceOffline(true);
    const bool detoursDeny = callsAfterInstall == 1
        && FakeHooks::offlineSetterCalls == 2
        && !FakeHooks::lastOfflineSetterValue
        && denyCall() == 0;
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

bool CleanupRetainsHooksAndRetriesEachFailedPhase() {
    FakeHooks::Reset();
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);
    if (InstallGameServiceGuardForTesting(configuration, Backend())
        != GameServiceGuardInstallStatus::Success) {
        return false;
    }

    FakeHooks::failDisableCall = 1;
    if (UninstallGameServiceGuard() != GameServiceGuardCleanupStatus::Incomplete
        || CurrentGameServiceGuardLifecycle().installedHookCount != 1
        || !CurrentGameServiceGuardLifecycle().cleanupIncomplete
        || !CurrentGameServiceGuardLifecycle().denyOnlyRetained
        || FakeHooks::removeCalls != 1) {
        return false;
    }
    FakeHooks::failDisableCall = 0;
    FakeHooks::failRemoveCall = FakeHooks::removeCalls + 1;
    if (UninstallGameServiceGuard() != GameServiceGuardCleanupStatus::Incomplete
        || CurrentGameServiceGuardLifecycle().installedHookCount != 1
        || !FakeHooks::acquired) {
        return false;
    }
    FakeHooks::failRemoveCall = 0;
    FakeHooks::failRelease = true;
    if (UninstallGameServiceGuard() != GameServiceGuardCleanupStatus::Incomplete
        || CurrentGameServiceGuardLifecycle().installedHookCount != 0
        || !FakeHooks::acquired) {
        return false;
    }
    FakeHooks::failRelease = false;
    return UninstallGameServiceGuard() == GameServiceGuardCleanupStatus::Success
        && !FakeHooks::acquired
        && !CurrentGameServiceGuardLifecycle().cleanupIncomplete;
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

bool BootstrapReportsCleanupFailureExactlyAndAllowsRetry() {
    FakeHooks::Reset();
    DSRRandomizer::Network::WinsockHookConfiguration winsock{};
    winsock.endpointCount = 1;
    winsock.endpoints[0].transport = DSRRandomizer::SocketTransport::Tcp;
    winsock.endpoints[0].family = AF_INET;
    winsock.endpoints[0].port = 1;
    winsock.endpoints[0].address[0] = 127;
    winsock.endpoints[0].address[3] = 1;
    if (DSRRandomizer::Network::InstallWinsockHooks(winsock)
        != DSRRandomizer::Network::WinsockHookInstallStatus::Success) {
        return false;
    }
    std::array<std::byte, 128> image{};
    auto configuration = Configuration(image);
    if (InstallGameServiceGuardForTesting(configuration, Backend())
        != GameServiceGuardInstallStatus::Success) {
        return false;
    }
    FakeHooks::failDisableCall = 1;

    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = sizeof(block);
    block.requiredFlags =
        static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::GameServiceOffline);
    const auto status = DSRRandomizer::InitializeForTest(&block);
    FakeHooks::failDisableCall = 0;
    const auto retried = UninstallGameServiceGuard();
    return status == DSRRandomizer::InitStatus::ProtectionCleanupFailed
        && retried == GameServiceGuardCleanupStatus::Success
        && DSRRandomizer::Hooks::Testing::MinHookReferenceCount() == 0;
}

bool ConcurrentProductionGroupsShareMutationAndOwnership() {
    const auto module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return false;
    }
    const auto* const base = reinterpret_cast<const std::byte*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    constexpr std::size_t fingerprintLength = 16;
    auto* const force = reinterpret_cast<const std::byte*>(
        &ProductionOfflineSetter);
    auto* const deny = reinterpret_cast<const std::byte*>(&ProductionDeniedCall);
    GameServiceGuardConfiguration game{
        {GameServiceImage{
            L"fixture.exe", base, nt->OptionalHeader.SizeOfImage}},
        {
            GameServiceTarget{
                L"fixture.exe",
                static_cast<std::uintptr_t>(force - base),
                Hash(std::span(force, fingerprintLength)),
                fingerprintLength,
                InternalTargetAction::ForceOffline},
            GameServiceTarget{
                L"fixture.exe",
                static_cast<std::uintptr_t>(deny - base),
                Hash(std::span(deny, fingerprintLength)),
                fingerprintLength,
                InternalTargetAction::DenyCall},
        },
    };
    DSRRandomizer::Network::WinsockHookConfiguration winsock{};
    winsock.endpointCount = 1;
    winsock.endpoints[0].transport = DSRRandomizer::SocketTransport::Tcp;
    winsock.endpoints[0].family = AF_INET;
    winsock.endpoints[0].port = 1;
    winsock.endpoints[0].address[0] = 127;
    winsock.endpoints[0].address[3] = 1;

    const HANDLE gameLeaseAcquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE allowGameInstall = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE winsockDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (gameLeaseAcquired == nullptr || allowGameInstall == nullptr
        || winsockDone == nullptr) {
        return false;
    }
    DSRRandomizer::Hooks::Testing::SetMutationLeasePauseEvents(
        gameLeaseAcquired, allowGameInstall);
    GameServiceGuardInstallStatus gameStatus{};
    DSRRandomizer::Network::WinsockHookInstallStatus winsockStatus{};
    std::thread gameThread([&]() {
        gameStatus = InstallGameServiceGuard(game);
    });
    const bool gameOwnsTransaction = WaitForSingleObject(
        gameLeaseAcquired, 2000) == WAIT_OBJECT_0;
    std::thread winsockThread([&]() {
        winsockStatus = DSRRandomizer::Network::InstallWinsockHooks(winsock);
        SetEvent(winsockDone);
    });
    const bool winsockBlocked = WaitForSingleObject(
        winsockDone, 100) == WAIT_TIMEOUT;
    SetEvent(allowGameInstall);
    gameThread.join();
    winsockThread.join();
    DSRRandomizer::Hooks::Testing::SetMutationLeasePauseEvents(nullptr, nullptr);
    CloseHandle(winsockDone);
    CloseHandle(allowGameInstall);
    CloseHandle(gameLeaseAcquired);
    ProductionOfflineSetter(true);
    const bool protectedOffline = !productionOfflineState;
    const bool installed = gameOwnsTransaction && winsockBlocked
        && gameStatus == GameServiceGuardInstallStatus::Success
        && winsockStatus
            == DSRRandomizer::Network::WinsockHookInstallStatus::Success
        && DSRRandomizer::Hooks::Testing::MinHookReferenceCount() == 2;
    const bool cleaned = UninstallGameServiceGuard()
            == GameServiceGuardCleanupStatus::Success
        && DSRRandomizer::Hooks::Testing::MinHookReferenceCount() == 1
        && DSRRandomizer::Network::UninstallWinsockHooks()
            == DSRRandomizer::Network::WinsockHookCleanupStatus::Success
        && DSRRandomizer::Hooks::Testing::MinHookReferenceCount() == 0;
    return installed && protectedOffline && cleaned;
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
        std::pair{"cleanup retains and retries failed phases",
                  &CleanupRetainsHooksAndRetriesEachFailedPhase},
        std::pair{"bootstrap rejects an unprofiled image",
                  &BootstrapFailsClosedWhenPinnedImageDoesNotMatch},
        std::pair{"bootstrap reports exact cleanup failure",
                  &BootstrapReportsCleanupFailureExactlyAndAllowsRetry},
        std::pair{"concurrent production groups share MinHook",
                  &ConcurrentProductionGroupsShareMutationAndOwnership},
    };

    for (const auto& [name, test] : tests) {
        if (!test()) {
            std::wcerr << L"FAIL: " << name << L'\n';
            return 1;
        }
    }
    return 0;
}

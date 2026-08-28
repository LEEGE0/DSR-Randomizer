#include "ProtectionBootstrap.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <string>

#include "game/GameServiceGuard.h"
#include "modules/DeferredModuleGate.h"
#include "monitor/ProtectionMonitor.h"
#include "network/WinsockHooks.h"
#include "profile/PinnedCompatibilityProfile.h"
#include "save/SaveHooks.h"

namespace DSRRandomizer {
namespace {

std::atomic<std::uint64_t> activeFlags{0};

bool IsZeroEndpoint(const ProtectionSocketEndpoint& endpoint) noexcept {
    return endpoint.transport == 0
        && endpoint.family == 0
        && endpoint.port == 0
        && endpoint.reserved == 0
        && std::all_of(
            std::begin(endpoint.address),
            std::end(endpoint.address),
            [](const std::uint8_t value) { return value == 0; });
}

bool ReadSocketConfiguration(
    const ProtectionInitBlock& block,
    Network::WinsockHookConfiguration& configuration) noexcept {
    if (block.socketEndpointCount > kProtectionSocketEndpointCapacity) {
        return false;
    }
    configuration.endpointCount = block.socketEndpointCount;
    for (std::size_t index = 0; index < kProtectionSocketEndpointCapacity; ++index) {
        const auto& source = block.socketEndpoints[index];
        if (index >= block.socketEndpointCount) {
            if (!IsZeroEndpoint(source)) {
                return false;
            }
            continue;
        }
        if (source.reserved != 0) {
            return false;
        }
        auto& destination = configuration.endpoints[index];
        destination.transport = static_cast<SocketTransport>(source.transport);
        destination.family = static_cast<ADDRESS_FAMILY>(source.family);
        destination.port = source.port;
        std::copy(std::begin(source.address), std::end(source.address),
            destination.address.begin());
    }
    return true;
}

bool SocketConfigurationIsEmpty(const ProtectionInitBlock& block) noexcept {
    return block.socketEndpointCount == 0
        && std::all_of(
            std::begin(block.socketEndpoints),
            std::end(block.socketEndpoints),
            [](const ProtectionSocketEndpoint& endpoint) {
                return IsZeroEndpoint(endpoint);
            });
}

bool UninstallProtectionGroups() noexcept {
    bool complete = Monitor::StopProtectionMonitor();
    complete = Game::UninstallGameServiceGuard()
            == Game::GameServiceGuardCleanupStatus::Success
        && complete;
    complete = Save::UninstallSaveHooks()
            == Save::SaveHookCleanupStatus::Success
        && complete;
    complete = Modules::UninstallDeferredModuleGate()
            == Modules::DeferredModuleGateCleanupStatus::Success
        && complete;
    complete = Network::UninstallWinsockHooks()
            == Network::WinsockHookCleanupStatus::Success
        && complete;
    return complete;
}

InitStatus CleanupOr(const InitStatus status) noexcept {
    return UninstallProtectionGroups()
        ? status
        : InitStatus::ProtectionCleanupFailed;
}

bool ReadRequiredPath(
    const wchar_t* source,
    const std::size_t capacity,
    std::wstring& destination) {
    const auto length = wcsnlen_s(source, capacity);
    if (length == 0 || length == capacity) {
        return false;
    }
    destination.assign(source, length);
    return true;
}

InitStatus InitializeCore(
    ProtectionInitBlock* block,
    const Testing::RequiredPathReader pathReader,
    const Testing::SteamConfigurationProvider steamProvider) noexcept {
    activeFlags.store(0, std::memory_order_release);
    if (block == nullptr || block->size != sizeof(ProtectionInitBlock)) {
        return InitStatus::InvalidArgument;
    }

    if (block->magic != kProtectionMagic
        || block->version != kProtectionProtocolVersion) {
        return InitStatus::UnsupportedProtocol;
    }

    constexpr auto bootstrapFlag =
        static_cast<std::uint64_t>(ProtectionFlags::Bootstrap);
    constexpr auto saveKnownFolderFlag =
        static_cast<std::uint64_t>(ProtectionFlags::SaveKnownFolder);
    constexpr auto saveFileIoFlag =
        static_cast<std::uint64_t>(ProtectionFlags::SaveFileIo);
    constexpr auto winsockFlag =
        static_cast<std::uint64_t>(ProtectionFlags::Winsock);
    constexpr auto steamInterfacesFlag =
        static_cast<std::uint64_t>(ProtectionFlags::SteamInterfaces);
    constexpr auto deferredModuleGateFlag =
        static_cast<std::uint64_t>(ProtectionFlags::DeferredModuleGate);
    constexpr auto gameServiceOfflineFlag =
        static_cast<std::uint64_t>(ProtectionFlags::GameServiceOffline);
    constexpr auto heartbeatFlag =
        static_cast<std::uint64_t>(ProtectionFlags::Heartbeat);
    constexpr auto hookIntegrityFlag =
        static_cast<std::uint64_t>(ProtectionFlags::HookIntegrity);
    constexpr auto saveFlags = saveKnownFolderFlag | saveFileIoFlag;
    constexpr auto steamFlags = steamInterfacesFlag | deferredModuleGateFlag;
    constexpr auto monitorFlags = heartbeatFlag | hookIntegrityFlag;
    constexpr auto supportedFlags =
        bootstrapFlag | saveFlags | winsockFlag | steamFlags
        | gameServiceOfflineFlag | monitorFlags;
    if ((block->requiredFlags & bootstrapFlag) == 0
        || (block->requiredFlags & ~supportedFlags) != 0
        || ((block->requiredFlags & saveFlags) != 0
            && (block->requiredFlags & saveFlags) != saveFlags)
        || ((block->requiredFlags & steamFlags) != 0
            && (block->requiredFlags & steamFlags) != steamFlags)
        || ((block->requiredFlags & monitorFlags) != 0
            && (block->requiredFlags & monitorFlags) != monitorFlags)) {
        return InitStatus::RequiredProtectionUnavailable;
    }

    if ((block->requiredFlags & winsockFlag) == 0) {
        if (!SocketConfigurationIsEmpty(*block)) {
            return InitStatus::InvalidArgument;
        }
    }
    else {
        Network::WinsockHookConfiguration socketConfiguration{};
        if (!ReadSocketConfiguration(*block, socketConfiguration)
            || Network::InstallWinsockHooks(socketConfiguration)
                != Network::WinsockHookInstallStatus::Success) {
            return CleanupOr(InitStatus::WinsockHookInstallFailed);
        }
    }

    std::optional<Profile::PinnedCompatibilityProfile> pinnedProfile;
    if (steamProvider == nullptr
        && (block->requiredFlags & (steamFlags | gameServiceOfflineFlag)) != 0) {
        pinnedProfile.emplace();
        if (Profile::BuildPinnedCompatibilityProfile(*pinnedProfile)
            != Profile::PinnedCompatibilityProfileStatus::Success) {
            return CleanupOr(
                (block->requiredFlags & gameServiceOfflineFlag) != 0
                    ? InitStatus::GameServiceProfileMismatch
                    : InitStatus::SteamConfigurationUnavailable);
        }
    }

    if ((block->requiredFlags & gameServiceOfflineFlag) != 0) {
        const auto gameStatus = pinnedProfile.has_value()
            ? Game::InstallGameServiceGuard(pinnedProfile->gameService)
            : Game::InstallPinnedGameServiceGuard();
        if (gameStatus == Game::GameServiceGuardInstallStatus::ProfileMismatch
            || gameStatus == Game::GameServiceGuardInstallStatus::InvalidConfiguration) {
            return CleanupOr(InitStatus::GameServiceProfileMismatch);
        }
        if (gameStatus != Game::GameServiceGuardInstallStatus::Success) {
            return CleanupOr(InitStatus::GameServiceHookFailed);
        }
    }

    if ((block->requiredFlags & steamFlags) == steamFlags) {
        try {
            Modules::DeferredModuleGateConfiguration configuration{};
            if (steamProvider != nullptr) {
                if (!steamProvider(configuration)) {
                    return CleanupOr(InitStatus::SteamConfigurationUnavailable);
                }
            }
            else if (pinnedProfile.has_value()) {
                configuration = pinnedProfile->steam;
            }
            else {
                return CleanupOr(InitStatus::SteamConfigurationUnavailable);
            }
            const auto gateStatus = steamProvider != nullptr
                ? Modules::Testing::
                    InstallDeferredModuleGateForSyntheticSuspendedProcess(
                        configuration)
                : Modules::InstallDeferredModuleGate(configuration);
            if (gateStatus
                != Modules::DeferredModuleGateInstallStatus::Success) {
                return CleanupOr(InitStatus::DeferredModuleGateInstallFailed);
            }
        }
        catch (...) {
            return CleanupOr(InitStatus::DeferredModuleGateInstallFailed);
        }
    }

    if ((block->requiredFlags & saveFlags) == saveFlags) {
        try {
            if (pathReader == nullptr) {
                return CleanupOr(InitStatus::SaveHookInstallFailed);
            }
            Save::SaveHookConfiguration configuration{};
            configuration.diagnosticMode = block->diagnosticMode != 0;
            if (!pathReader(
                    block->virtualDocuments,
                    kProtectionSavePathCharacters,
                    configuration.virtualDocuments)
                || !pathReader(
                    block->virtualLogicalSave,
                    kProtectionSavePathCharacters,
                    configuration.virtualLogicalSave)
                || !pathReader(
                    block->realSaveRoot,
                    kProtectionSavePathCharacters,
                    configuration.realSaveRoot)
                || !pathReader(
                    block->externalSaveRoot,
                    kProtectionSavePathCharacters,
                    configuration.externalSaveRoot)
                || !pathReader(
                    block->dedicatedRmm,
                    kProtectionSavePathCharacters,
                    configuration.dedicatedRmm)
                || Save::InstallSaveHooks(configuration)
                    != Save::SaveHookInstallStatus::Success) {
                return CleanupOr(InitStatus::SaveHookInstallFailed);
            }
        }
        catch (...) {
            return CleanupOr(InitStatus::SaveHookInstallFailed);
        }
    }

    activeFlags.store(block->requiredFlags, std::memory_order_release);
    return InitStatus::Success;
}

InitStatus OpenSupervisorSession(
    const ProtectionInitBlock& block,
    HANDLE& pipe) noexcept {
    pipe = INVALID_HANDLE_VALUE;
    const auto pipeNameLength = wcsnlen_s(
        block.pipeName,
        kProtectionPipeNameCharacters);
    if (pipeNameLength == 0 || pipeNameLength == kProtectionPipeNameCharacters) {
        return InitStatus::InvalidArgument;
    }

    if (!WaitNamedPipeW(block.pipeName, 10000)) {
        return InitStatus::SupervisorUnavailable;
    }

    pipe = CreateFileW(
        block.pipeName,
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return InitStatus::SupervisorUnavailable;
    }

    ProtectionHandshakeMessage message{};
    message.magic = kProtectionMagic;
    message.version = kProtectionProtocolVersion;
    message.size = static_cast<std::uint16_t>(sizeof(message));
    std::copy_n(block.nonce, kProtectionNonceSize, message.nonce);
    message.kind = static_cast<std::uint32_t>(
        ProtectionMessageKind::Handshake);
    message.status = static_cast<std::uint32_t>(InitStatus::Success);
    message.activeFlags = activeFlags.load(std::memory_order_acquire);

    DWORD bytesWritten = 0;
    const BOOL wrote = WriteFile(
        pipe,
        &message,
        static_cast<DWORD>(sizeof(message)),
        &bytesWritten,
        nullptr);
    const BOOL flushed = wrote ? FlushFileBuffers(pipe) : FALSE;
    if (!wrote || !flushed || bytesWritten != sizeof(message)) {
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        return InitStatus::SupervisorReportFailed;
    }

    return InitStatus::Success;
}

std::uint64_t ReadCurrentProtectionFlags() noexcept {
    return activeFlags.load(std::memory_order_acquire);
}

}  // namespace

InitStatus InitializeProtection(ProtectionInitBlock* block) noexcept {
    if (block != nullptr
        && (block->requiredFlags == kDedicatedSaveRequiredFlags
            || block->requiredFlags == kSimplifiedOfflineRequiredFlags)) {
        const auto status = InitializeCore(block, &ReadRequiredPath, nullptr);
        if (status != InitStatus::Success) {
            return status;
        }

        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto reportStatus = OpenSupervisorSession(*block, pipe);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        return reportStatus;
    }

    constexpr auto monitorFlags =
        static_cast<std::uint64_t>(ProtectionFlags::Heartbeat)
        | static_cast<std::uint64_t>(ProtectionFlags::HookIntegrity);
    if (block == nullptr
        || (block->requiredFlags & monitorFlags) != monitorFlags) {
        return InitStatus::RequiredProtectionUnavailable;
    }
    const auto status = InitializeCore(block, &ReadRequiredPath, nullptr);
    if (status != InitStatus::Success) {
        return status;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto reportStatus = OpenSupervisorSession(*block, pipe);
    if (reportStatus != InitStatus::Success) {
        activeFlags.store(0, std::memory_order_release);
        if (!UninstallProtectionGroups()) {
            return InitStatus::ProtectionCleanupFailed;
        }
        return reportStatus;
    }

    std::array<std::uint8_t, kProtectionNonceSize> nonce{};
    std::copy_n(block->nonce, kProtectionNonceSize, nonce.begin());
    if (!Monitor::StartProtectionMonitor(
            pipe, nonce, &ReadCurrentProtectionFlags)) {
        CloseHandle(pipe);
        activeFlags.store(0, std::memory_order_release);
        return UninstallProtectionGroups()
            ? InitStatus::SupervisorReportFailed
            : InitStatus::ProtectionCleanupFailed;
    }

    return reportStatus;
}

InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept {
    return InitializeCore(block, &ReadRequiredPath, nullptr);
}

ProtectionFlags CurrentProtectionFlags() noexcept {
    return static_cast<ProtectionFlags>(activeFlags.load(std::memory_order_acquire));
}

namespace Testing {

InitStatus InitializeWithPathReader(
    ProtectionInitBlock* block,
    const RequiredPathReader reader) noexcept {
    return InitializeCore(block, reader, nullptr);
}

InitStatus InitializeWithSteamConfigurationProvider(
    ProtectionInitBlock* block,
    const SteamConfigurationProvider provider) noexcept {
    return InitializeCore(block, &ReadRequiredPath, provider);
}

}  // namespace Testing

}  // namespace DSRRandomizer

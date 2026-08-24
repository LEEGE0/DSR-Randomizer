#include "ProtectionBootstrap.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "save/SaveHooks.h"

namespace DSRRandomizer {
namespace {

std::atomic<std::uint64_t> activeFlags{0};

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
    const Testing::RequiredPathReader pathReader) noexcept {
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
    constexpr auto saveFlags = saveKnownFolderFlag | saveFileIoFlag;
    constexpr auto supportedFlags = bootstrapFlag | saveFlags;
    if ((block->requiredFlags & bootstrapFlag) == 0
        || (block->requiredFlags & ~supportedFlags) != 0
        || ((block->requiredFlags & saveFlags) != 0
            && (block->requiredFlags & saveFlags) != saveFlags)) {
        return InitStatus::RequiredProtectionUnavailable;
    }

    if ((block->requiredFlags & saveFlags) == saveFlags) {
        try {
            if (pathReader == nullptr) {
                static_cast<void>(Save::UninstallSaveHooks());
                return InitStatus::SaveHookInstallFailed;
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
                static_cast<void>(Save::UninstallSaveHooks());
                return InitStatus::SaveHookInstallFailed;
            }
        }
        catch (...) {
            static_cast<void>(Save::UninstallSaveHooks());
            return InitStatus::SaveHookInstallFailed;
        }
    }

    activeFlags.store(block->requiredFlags, std::memory_order_release);
    return InitStatus::Success;
}

InitStatus ReportHandshake(const ProtectionInitBlock& block) noexcept {
    const auto pipeNameLength = wcsnlen_s(
        block.pipeName,
        kProtectionPipeNameCharacters);
    if (pipeNameLength == 0 || pipeNameLength == kProtectionPipeNameCharacters) {
        return InitStatus::InvalidArgument;
    }

    if (!WaitNamedPipeW(block.pipeName, 10000)) {
        return InitStatus::SupervisorUnavailable;
    }

    const HANDLE pipe = CreateFileW(
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
    CloseHandle(pipe);
    if (!wrote || !flushed || bytesWritten != sizeof(message)) {
        return InitStatus::SupervisorReportFailed;
    }

    return InitStatus::Success;
}

}  // namespace

InitStatus InitializeProtection(ProtectionInitBlock* block) noexcept {
    const auto status = InitializeCore(block, &ReadRequiredPath);
    if (status != InitStatus::Success) {
        return status;
    }

    const auto reportStatus = ReportHandshake(*block);
    if (reportStatus != InitStatus::Success) {
        activeFlags.store(0, std::memory_order_release);
        static_cast<void>(Save::UninstallSaveHooks());
    }

    return reportStatus;
}

InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept {
    return InitializeCore(block, &ReadRequiredPath);
}

ProtectionFlags CurrentProtectionFlags() noexcept {
    return static_cast<ProtectionFlags>(activeFlags.load(std::memory_order_acquire));
}

namespace Testing {

InitStatus InitializeWithPathReader(
    ProtectionInitBlock* block,
    const RequiredPathReader reader) noexcept {
    return InitializeCore(block, reader);
}

}  // namespace Testing

}  // namespace DSRRandomizer

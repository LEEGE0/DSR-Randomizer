#include "ProtectionBootstrap.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>

namespace DSRRandomizer {
namespace {

std::atomic<std::uint64_t> activeFlags{0};

InitStatus InitializeCore(ProtectionInitBlock* block) noexcept {
    activeFlags.store(0, std::memory_order_release);
    if (block == nullptr || block->size != sizeof(ProtectionInitBlock)) {
        return InitStatus::InvalidArgument;
    }

    if (block->magic != kProtectionMagic
        || block->version != kProtectionProtocolVersion) {
        return InitStatus::UnsupportedProtocol;
    }

    constexpr auto installedFlags =
        static_cast<std::uint64_t>(ProtectionFlags::Bootstrap);
    if (block->requiredFlags != installedFlags) {
        return InitStatus::RequiredProtectionUnavailable;
    }

    activeFlags.store(installedFlags, std::memory_order_release);
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
    const auto status = InitializeCore(block);
    if (status != InitStatus::Success) {
        return status;
    }

    const auto reportStatus = ReportHandshake(*block);
    if (reportStatus != InitStatus::Success) {
        activeFlags.store(0, std::memory_order_release);
    }

    return reportStatus;
}

InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept {
    return InitializeCore(block);
}

ProtectionFlags CurrentProtectionFlags() noexcept {
    return static_cast<ProtectionFlags>(activeFlags.load(std::memory_order_acquire));
}

}  // namespace DSRRandomizer

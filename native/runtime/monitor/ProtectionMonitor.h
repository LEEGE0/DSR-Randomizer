#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <Windows.h>

#include "DSRRandomizer/ProtectionProtocol.h"

namespace DSRRandomizer::Monitor {

struct DeniedCounterSnapshot {
    std::array<std::uint64_t, kProtectionDeniedCounterCount> values{};
};

enum class ProtectionWaitResult {
    Elapsed,
    StopRequested,
    Failed,
};

enum class ProtectionMonitorRunResult {
    Stopped,
    HookIntegrityFailed,
    HeartbeatStopped,
    ProtectionThreadFailed,
};

class ProtectionMonitorPlatform {
public:
    virtual ~ProtectionMonitorPlatform() = default;
    virtual bool VerifyInstalledHooks() = 0;
    virtual bool QueryMonotonicMilliseconds(std::uint64_t& value) = 0;
    virtual std::uint64_t CurrentActiveFlags() = 0;
    virtual DeniedCounterSnapshot CurrentDeniedCounters() = 0;
    virtual bool WriteMessage(const void* message, std::size_t size) = 0;
    virtual ProtectionWaitResult WaitOneSecondOrStop() = 0;
};

class ProtectionMonitor final {
public:
    explicit ProtectionMonitor(
        std::array<std::uint8_t, kProtectionNonceSize> nonce) noexcept;

    [[nodiscard]] ProtectionMonitorRunResult Run(
        ProtectionMonitorPlatform& platform) noexcept;

private:
    bool SendFatal(
        ProtectionMonitorPlatform& platform,
        ProtectionFatalCode code) noexcept;

    std::array<std::uint8_t, kProtectionNonceSize> nonce_{};
};

using CurrentProtectionFlagsReader = std::uint64_t(*)() noexcept;

[[nodiscard]] bool StartProtectionMonitor(
    HANDLE pipe,
    std::array<std::uint8_t, kProtectionNonceSize> nonce,
    CurrentProtectionFlagsReader flagsReader) noexcept;
[[nodiscard]] bool StopProtectionMonitor() noexcept;
[[nodiscard]] bool ProtectionMonitorIsRunning() noexcept;

}  // namespace DSRRandomizer::Monitor

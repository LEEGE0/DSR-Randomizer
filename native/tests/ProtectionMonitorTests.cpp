#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <Windows.h>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "monitor/HookIntegrityRegistry.h"
#include "monitor/ProtectionMonitor.h"

namespace {

using DSRRandomizer::ProtectionFatalCode;
using DSRRandomizer::ProtectionFlags;
using DSRRandomizer::ProtectionHeartbeatMessage;
using DSRRandomizer::ProtectionMessageKind;
using DSRRandomizer::Monitor::DeniedCounterSnapshot;
using DSRRandomizer::Monitor::ProtectionMonitor;
using DSRRandomizer::Monitor::ProtectionMonitorPlatform;
using DSRRandomizer::Monitor::ProtectionMonitorRunResult;
using DSRRandomizer::Monitor::ProtectionWaitResult;

constexpr std::uint64_t kExpectedFlags = 0x183;

std::uint64_t ReadExpectedFlags() noexcept {
    return kExpectedFlags;
}

class FakePlatform final : public ProtectionMonitorPlatform {
public:
    bool VerifyInstalledHooks() override {
        if (throwDuringVerification) {
            throw std::runtime_error("synthetic protection-thread failure");
        }
        return hooksIntact;
    }

    bool QueryMonotonicMilliseconds(std::uint64_t& value) override {
        value = monotonicMilliseconds;
        return clockAvailable;
    }

    std::uint64_t CurrentActiveFlags() override {
        return activeFlags;
    }

    DeniedCounterSnapshot CurrentDeniedCounters() override {
        return counters;
    }

    bool WriteMessage(const void* const message, const std::size_t size) override {
        const auto* const bytes = static_cast<const std::byte*>(message);
        messages.emplace_back(bytes, bytes + size);
        return writeSucceeds;
    }

    ProtectionWaitResult WaitOneSecondOrStop() override {
        if (waitsBeforeStop-- > 0) {
            monotonicMilliseconds += 1'000;
            return ProtectionWaitResult::Elapsed;
        }
        return waitResult;
    }

    bool hooksIntact = true;
    bool throwDuringVerification = false;
    bool clockAvailable = true;
    bool writeSucceeds = true;
    int waitsBeforeStop = 0;
    ProtectionWaitResult waitResult = ProtectionWaitResult::StopRequested;
    std::uint64_t monotonicMilliseconds = 10'000;
    std::uint64_t activeFlags = kExpectedFlags;
    DeniedCounterSnapshot counters{{1, 2, 3, 4, 5, 6}};
    std::vector<std::vector<std::byte>> messages;
};

std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize> Nonce() {
    std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize> nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::uint8_t>(index);
    }
    return nonce;
}

bool IsFatal(
    const std::vector<std::byte>& bytes,
    const ProtectionFatalCode expected) {
    if (bytes.size() != sizeof(DSRRandomizer::ProtectionFatalMessage)) {
        return false;
    }
    DSRRandomizer::ProtectionFatalMessage message{};
    std::memcpy(&message, bytes.data(), sizeof(message));
    return message.magic == DSRRandomizer::kProtectionMagic
        && message.version == DSRRandomizer::kProtectionProtocolVersion
        && message.size == sizeof(message)
        && message.kind == static_cast<std::uint32_t>(ProtectionMessageKind::Fatal)
        && message.fatalCode == static_cast<std::uint32_t>(expected)
        && std::equal(std::begin(message.nonce), std::end(message.nonce), Nonce().begin());
}

int HeartbeatCarriesExactState() {
    FakePlatform platform;
    ProtectionMonitor monitor(Nonce());

    const auto result = monitor.Run(platform);

    if (result != ProtectionMonitorRunResult::Stopped
        || platform.messages.size() != 1
        || platform.messages.front().size() != sizeof(ProtectionHeartbeatMessage)) {
        return 1;
    }
    ProtectionHeartbeatMessage message{};
    std::memcpy(&message, platform.messages.front().data(), sizeof(message));
    return message.magic == DSRRandomizer::kProtectionMagic
            && message.version == DSRRandomizer::kProtectionProtocolVersion
            && message.size == sizeof(message)
            && message.kind == static_cast<std::uint32_t>(ProtectionMessageKind::Heartbeat)
            && message.sequence == 1
            && message.monotonicMilliseconds == 10'000
            && message.activeFlags == kExpectedFlags
            && std::equal(
                std::begin(message.deniedCounters),
                std::end(message.deniedCounters),
                platform.counters.values.begin())
            && std::equal(
                std::begin(message.nonce),
                std::end(message.nonce),
                Nonce().begin())
        ? 0
        : 1;
}

int TargetAndTrampolineMutationsFailIntegrity() {
    std::array<std::byte, 32> target{};
    std::array<std::byte, 32> trampoline{};
    for (std::size_t index = 0; index < target.size(); ++index) {
        target[index] = static_cast<std::byte>(index + 1);
        trampoline[index] = static_cast<std::byte>(index + 65);
    }
    DSRRandomizer::Monitor::ClearHookIntegrityRegistryForTesting();
    if (!DSRRandomizer::Monitor::RegisterInstalledHook(
            target.data(), trampoline.data(), 16)
        || !DSRRandomizer::Monitor::VerifyAllInstalledHooks()
        || DSRRandomizer::Monitor::InstalledHookCount() != 1) {
        return 1;
    }

    target[3] ^= std::byte{0x7f};
    const bool targetDetected = !DSRRandomizer::Monitor::VerifyAllInstalledHooks();
    target[3] ^= std::byte{0x7f};
    trampoline[9] ^= std::byte{0x55};
    const bool trampolineDetected = !DSRRandomizer::Monitor::VerifyAllInstalledHooks();
    trampoline[9] ^= std::byte{0x55};
    DSRRandomizer::Monitor::UnregisterInstalledHook(target.data());
    const bool clean = DSRRandomizer::Monitor::VerifyAllInstalledHooks()
        && DSRRandomizer::Monitor::InstalledHookCount() == 0;
    return targetDetected && trampolineDetected && clean ? 0 : 1;
}

int HookMutationSendsFatalBeforeAnotherHeartbeat() {
    FakePlatform platform;
    platform.hooksIntact = false;
    ProtectionMonitor monitor(Nonce());

    const auto result = monitor.Run(platform);

    return result == ProtectionMonitorRunResult::HookIntegrityFailed
            && platform.messages.size() == 1
            && IsFatal(platform.messages.front(), ProtectionFatalCode::HookIntegrityFailed)
        ? 0
        : 1;
}

int FailedWaitSendsHeartbeatStopped() {
    FakePlatform platform;
    platform.waitResult = ProtectionWaitResult::Failed;
    ProtectionMonitor monitor(Nonce());

    const auto result = monitor.Run(platform);

    return result == ProtectionMonitorRunResult::HeartbeatStopped
            && platform.messages.size() == 2
            && IsFatal(platform.messages.back(), ProtectionFatalCode::HeartbeatStopped)
        ? 0
        : 1;
}

int ThrownThreadFailureIsCaughtAndReported() {
    FakePlatform platform;
    platform.throwDuringVerification = true;
    ProtectionMonitor monitor(Nonce());

    const auto result = monitor.Run(platform);

    return result == ProtectionMonitorRunResult::ProtectionThreadFailed
            && platform.messages.size() == 1
            && IsFatal(platform.messages.front(), ProtectionFatalCode::ProtectionThreadFailed)
        ? 0
        : 1;
}

int NativeThreadStartsBeforeReturningAndStopsCleanly() {
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, nullptr, 512)) {
        return 1;
    }
    const auto started = DSRRandomizer::Monitor::StartProtectionMonitor(
        writePipe,
        Nonce(),
        &ReadExpectedFlags);
    if (!started) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return 1;
    }

    ProtectionHeartbeatMessage heartbeat{};
    DWORD read = 0;
    const bool received = ReadFile(
            readPipe,
            &heartbeat,
            static_cast<DWORD>(sizeof(heartbeat)),
            &read,
            nullptr)
        && read == sizeof(heartbeat)
        && heartbeat.kind == static_cast<std::uint32_t>(
            ProtectionMessageKind::Heartbeat)
        && heartbeat.sequence == 1
        && heartbeat.activeFlags == kExpectedFlags;
    const bool running = DSRRandomizer::Monitor::ProtectionMonitorIsRunning();
    const bool stopped = DSRRandomizer::Monitor::StopProtectionMonitor()
        && !DSRRandomizer::Monitor::ProtectionMonitorIsRunning();
    CloseHandle(readPipe);
    return received && running && stopped ? 0 : 1;
}

}  // namespace

int main() {
    const std::array tests{
        std::pair{"heartbeat carries exact state", &HeartbeatCarriesExactState},
        std::pair{"target and trampoline mutation", &TargetAndTrampolineMutationsFailIntegrity},
        std::pair{"hook mutation fatal", &HookMutationSendsFatalBeforeAnotherHeartbeat},
        std::pair{"heartbeat stopped fatal", &FailedWaitSendsHeartbeatStopped},
        std::pair{"thread failure fatal", &ThrownThreadFailureIsCaughtAndReported},
        std::pair{"native monitor thread lifecycle", &NativeThreadStartsBeforeReturningAndStopsCleanly},
    };
    for (const auto& [name, test] : tests) {
        if (test() != 0) {
            std::cerr << name << " failed\n";
            return 1;
        }
    }
    return 0;
}

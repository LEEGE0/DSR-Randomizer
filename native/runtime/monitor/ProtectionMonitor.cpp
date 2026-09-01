#include "monitor/ProtectionMonitor.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#include <Windows.h>

#include "monitor/HookIntegrityRegistry.h"
#include "network/WinsockHooks.h"
#include "save/SaveHooks.h"

namespace DSRRandomizer::Monitor {
namespace {

class NativeProtectionMonitorPlatform final : public ProtectionMonitorPlatform {
public:
    NativeProtectionMonitorPlatform(
        const HANDLE pipe,
        const HANDLE stopEvent,
        const CurrentProtectionFlagsReader flagsReader) noexcept
        : pipe_(pipe), stopEvent_(stopEvent), flagsReader_(flagsReader) {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency)) {
            frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
        }
    }

    bool VerifyInstalledHooks() override {
        return VerifyAllInstalledHooks();
    }

    bool QueryMonotonicMilliseconds(std::uint64_t& value) override {
        LARGE_INTEGER counter{};
        if (frequency_ == 0 || !QueryPerformanceCounter(&counter)
            || counter.QuadPart < 0) {
            return false;
        }
        const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
        value = (ticks / frequency_) * 1'000
            + ((ticks % frequency_) * 1'000) / frequency_;
        return true;
    }

    std::uint64_t CurrentActiveFlags() override {
        return flagsReader_ == nullptr ? 0 : flagsReader_();
    }

    DeniedCounterSnapshot CurrentDeniedCounters() override {
        const auto winsock = Network::CurrentWinsockAuditCounters();
        const auto save = Save::CurrentSaveAuditCounters();
        return {{
            winsock.connect,
            winsock.wsaConnect,
            winsock.sendTo,
            winsock.connectEx,
            save.deniedNormal,
            save.deniedOverhaul,
        }};
    }

    bool WriteMessage(const void* const message, const std::size_t size) override {
        if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE
            || message == nullptr || size > MAXDWORD) {
            return false;
        }
        DWORD written = 0;
        return WriteFile(
                pipe_,
                message,
                static_cast<DWORD>(size),
                &written,
                nullptr)
            && written == size;
    }

    ProtectionWaitResult WaitOneSecondOrStop() override {
        const auto result = WaitForSingleObject(stopEvent_, 1'000);
        if (result == WAIT_TIMEOUT) {
            return ProtectionWaitResult::Elapsed;
        }
        return result == WAIT_OBJECT_0
            ? ProtectionWaitResult::StopRequested
            : ProtectionWaitResult::Failed;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;
    CurrentProtectionFlagsReader flagsReader_ = nullptr;
    std::uint64_t frequency_ = 0;
};

struct NativeMonitorSession {
    NativeMonitorSession(
        const HANDLE pipeValue,
        const HANDLE stopEventValue,
        const std::array<std::uint8_t, kProtectionNonceSize>& nonce,
        const CurrentProtectionFlagsReader flagsReader) noexcept
        : pipe(pipeValue),
          stopEvent(stopEventValue),
          platform(pipeValue, stopEventValue, flagsReader),
          monitor(nonce) {}

    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> running{true};
    NativeProtectionMonitorPlatform platform;
    ProtectionMonitor monitor;
};

std::mutex sessionMutex;
std::unique_ptr<NativeMonitorSession> activeSession;

DWORD WINAPI ProtectionThreadEntry(void* const parameter) noexcept {
    auto* const session = static_cast<NativeMonitorSession*>(parameter);
    if (session != nullptr) {
        static_cast<void>(session->monitor.Run(session->platform));
        session->running.store(false, std::memory_order_release);
    }
    return 0;
}

}  // namespace

ProtectionMonitor::ProtectionMonitor(
    std::array<std::uint8_t, kProtectionNonceSize> nonce) noexcept
    : nonce_(nonce) {}

ProtectionMonitorRunResult ProtectionMonitor::Run(
    ProtectionMonitorPlatform& platform) noexcept {
    std::uint64_t sequence = 0;
    try {
        while (true) {
            if (!platform.VerifyInstalledHooks()) {
                static_cast<void>(SendFatal(
                    platform, ProtectionFatalCode::HookIntegrityFailed));
                return ProtectionMonitorRunResult::HookIntegrityFailed;
            }

            std::uint64_t monotonicMilliseconds = 0;
            if (!platform.QueryMonotonicMilliseconds(monotonicMilliseconds)) {
                static_cast<void>(SendFatal(
                    platform, ProtectionFatalCode::ProtectionThreadFailed));
                return ProtectionMonitorRunResult::ProtectionThreadFailed;
            }

            ProtectionHeartbeatMessage heartbeat{};
            heartbeat.magic = kProtectionMagic;
            heartbeat.version = kProtectionProtocolVersion;
            heartbeat.size = static_cast<std::uint16_t>(sizeof(heartbeat));
            std::copy(nonce_.begin(), nonce_.end(), heartbeat.nonce);
            heartbeat.kind = static_cast<std::uint32_t>(
                ProtectionMessageKind::Heartbeat);
            heartbeat.sequence = ++sequence;
            heartbeat.monotonicMilliseconds = monotonicMilliseconds;
            heartbeat.activeFlags = platform.CurrentActiveFlags();
            const auto counters = platform.CurrentDeniedCounters();
            std::copy(
                counters.values.begin(),
                counters.values.end(),
                heartbeat.deniedCounters);
            if (!platform.WriteMessage(&heartbeat, sizeof(heartbeat))) {
                return ProtectionMonitorRunResult::HeartbeatStopped;
            }

            switch (platform.WaitOneSecondOrStop()) {
            case ProtectionWaitResult::Elapsed:
                break;
            case ProtectionWaitResult::StopRequested:
                return ProtectionMonitorRunResult::Stopped;
            case ProtectionWaitResult::Failed:
                static_cast<void>(SendFatal(
                    platform, ProtectionFatalCode::HeartbeatStopped));
                return ProtectionMonitorRunResult::HeartbeatStopped;
            }
        }
    }
    catch (...) {
        static_cast<void>(SendFatal(
            platform, ProtectionFatalCode::ProtectionThreadFailed));
        return ProtectionMonitorRunResult::ProtectionThreadFailed;
    }
}

bool ProtectionMonitor::SendFatal(
    ProtectionMonitorPlatform& platform,
    const ProtectionFatalCode code) noexcept {
    try {
        ProtectionFatalMessage fatal{};
        fatal.magic = kProtectionMagic;
        fatal.version = kProtectionProtocolVersion;
        fatal.size = static_cast<std::uint16_t>(sizeof(fatal));
        std::copy(nonce_.begin(), nonce_.end(), fatal.nonce);
        fatal.kind = static_cast<std::uint32_t>(ProtectionMessageKind::Fatal);
        fatal.fatalCode = static_cast<std::uint32_t>(code);
        return platform.WriteMessage(&fatal, sizeof(fatal));
    }
    catch (...) {
        return false;
    }
}

bool StartProtectionMonitor(
    const HANDLE pipe,
    const std::array<std::uint8_t, kProtectionNonceSize> nonce,
    const CurrentProtectionFlagsReader flagsReader) noexcept {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || flagsReader == nullptr) {
        return false;
    }

    HANDLE stopEvent = nullptr;
    try {
        std::scoped_lock lock(sessionMutex);
        if (activeSession != nullptr) {
            return false;
        }
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr) {
            return false;
        }
        auto session = std::make_unique<NativeMonitorSession>(
            pipe, stopEvent, nonce, flagsReader);
        session->thread = CreateThread(
            nullptr,
            0,
            &ProtectionThreadEntry,
            session.get(),
            0,
            nullptr);
        if (session->thread == nullptr) {
            CloseHandle(stopEvent);
            return false;
        }
        activeSession = std::move(session);
        stopEvent = nullptr;
        return true;
    }
    catch (...) {
        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
        }
        return false;
    }
}

bool StopProtectionMonitor() noexcept {
    std::unique_ptr<NativeMonitorSession> session;
    {
        std::scoped_lock lock(sessionMutex);
        session = std::move(activeSession);
    }
    if (session == nullptr) {
        return true;
    }

    bool complete = SetEvent(session->stopEvent) != FALSE;
    const auto wait = WaitForSingleObject(session->thread, 5'000);
    if (wait != WAIT_OBJECT_0) {
        std::scoped_lock lock(sessionMutex);
        activeSession = std::move(session);
        return false;
    }
    CloseHandle(session->thread);
    CloseHandle(session->stopEvent);
    CloseHandle(session->pipe);
    return complete;
}

bool ProtectionMonitorIsRunning() noexcept {
    std::scoped_lock lock(sessionMutex);
    return activeSession != nullptr
        && activeSession->running.load(std::memory_order_acquire);
}

}  // namespace DSRRandomizer::Monitor

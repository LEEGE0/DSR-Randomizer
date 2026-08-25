#pragma once

#include <winsock2.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "DSRRandomizer/ProtectionProtocol.h"

namespace DSRRandomizer::Network {

struct AllowedSocketEndpoint {
    SocketTransport transport;
    ADDRESS_FAMILY family;
    std::uint16_t port;
    std::array<std::uint8_t, 16> address;
};

struct WinsockHookConfiguration {
    std::array<AllowedSocketEndpoint, kProtectionSocketEndpointCapacity> endpoints{};
    std::size_t endpointCount = 0;
};

enum class WinsockHookInstallStatus {
    Success,
    InvalidConfiguration,
    InstallFailed,
};

enum class WinsockHookCleanupStatus {
    Success,
    Incomplete,
};

struct WinsockAuditCounters {
    std::uint64_t connect;
    std::uint64_t wsaConnect;
    std::uint64_t sendTo;
    std::uint64_t connectEx;
    std::uint64_t total;
};

class HookPlatform {
public:
    virtual ~HookPlatform() = default;

    virtual void BeginMutation() noexcept {}
    virtual void EndMutation() noexcept {}

    virtual bool Initialize() noexcept = 0;
    virtual void* ResolveTarget(
        const wchar_t* moduleName,
        const char* procedureName) noexcept = 0;
    virtual bool CreateHook(
        void* target,
        void* detour,
        void** original) noexcept = 0;
    virtual bool QueueEnable(void* target) noexcept = 0;
    virtual bool ApplyQueued() noexcept = 0;
    virtual bool DisableAll() noexcept = 0;
    virtual bool RemoveHook(void* target) noexcept = 0;
    virtual bool Uninitialize() noexcept = 0;
};

[[nodiscard]] WinsockHookInstallStatus InstallWinsockHooks(
    const WinsockHookConfiguration& configuration) noexcept;
[[nodiscard]] WinsockHookInstallStatus InstallWinsockHooks(
    const WinsockHookConfiguration& configuration,
    HookPlatform& platform) noexcept;
[[nodiscard]] WinsockHookCleanupStatus UninstallWinsockHooks() noexcept;
[[nodiscard]] bool WinsockHooksAreInstalled() noexcept;
[[nodiscard]] WinsockAuditCounters CurrentWinsockAuditCounters() noexcept;

namespace Testing {

void HoldWinsockHookCallbackWhileWaitingForMutation(
    void* enteredEvent,
    void* allowMutationEvent,
    void* mutationAcquiredEvent,
    void* releaseEvent) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer::Network

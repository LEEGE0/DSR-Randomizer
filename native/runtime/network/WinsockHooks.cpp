#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "network/NetworkPolicy.h"
#include "network/WinsockHooks.h"

namespace DSRRandomizer::Network {
namespace {

using ConnectFunction = decltype(&connect);
using WsaConnectFunction = decltype(&WSAConnect);
using SendToFunction = decltype(&sendto);
using WsaIoctlFunction = decltype(&WSAIoctl);

struct HookTrampolines {
    ConnectFunction connect = nullptr;
    WsaConnectFunction wsaConnect = nullptr;
    SendToFunction sendTo = nullptr;
    WsaIoctlFunction wsaIoctl = nullptr;
};

struct HookContext {
    explicit HookContext(WinsockHookConfiguration value)
        : configuration(std::move(value)) {}

    WinsockHookConfiguration configuration;
    HookTrampolines trampolines;
    std::atomic<bool> denyOnly{false};
    std::atomic<std::uint64_t> inFlight{0};
};

struct HookLifecycle {
    HookPlatform* platform = nullptr;
    std::shared_ptr<HookContext> context;
    std::array<void*, 4> targets{};
    std::array<bool, 4> created{};
    bool initialized = false;
    bool mayBeEnabled = false;
};

std::mutex installMutex;
std::shared_mutex callbackGate;
std::atomic<std::shared_ptr<HookContext>> activeContext;
HookLifecycle lifecycle{};
std::atomic<bool> hooksInstalled{false};
std::array<std::atomic<std::uint64_t>, 4> deniedCounters{};

class CallbackLease final {
public:
    CallbackLease()
        : gate_(callbackGate),
          context_(activeContext.load(std::memory_order_acquire)) {
        if (context_ != nullptr) {
            context_->inFlight.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    ~CallbackLease() {
        if (context_ != nullptr) {
            context_->inFlight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    [[nodiscard]] const std::shared_ptr<HookContext>& Context() const noexcept {
        return context_;
    }

private:
    std::shared_lock<std::shared_mutex> gate_;
    std::shared_ptr<HookContext> context_;
};

std::size_t CounterIndex(const SocketOperation operation) noexcept {
    switch (operation) {
    case SocketOperation::Connect:
        return 0;
    case SocketOperation::WsaConnect:
        return 1;
    case SocketOperation::SendTo:
        return 2;
    case SocketOperation::ConnectEx:
        return 3;
    }
    return 0;
}

void ReportDenied(const SocketOperation operation) noexcept {
    deniedCounters[CounterIndex(operation)].fetch_add(1, std::memory_order_relaxed);
}

bool ReadSocketIdentity(
    const SOCKET socketHandle,
    const SocketOperation operation,
    SocketTransport& transport,
    ADDRESS_FAMILY& family) noexcept {
    WSAPROTOCOL_INFOW protocol{};
    int protocolLength = sizeof(protocol);
    if (getsockopt(
            socketHandle,
            SOL_SOCKET,
            SO_PROTOCOL_INFOW,
            reinterpret_cast<char*>(&protocol),
            &protocolLength) == SOCKET_ERROR
        || protocolLength != sizeof(protocol)) {
        return false;
    }

    family = static_cast<ADDRESS_FAMILY>(protocol.iAddressFamily);
    if (protocol.iSocketType == SOCK_STREAM
        && protocol.iProtocol == IPPROTO_TCP
        && operation != SocketOperation::SendTo) {
        transport = SocketTransport::Tcp;
        return true;
    }
    if (protocol.iSocketType == SOCK_DGRAM
        && protocol.iProtocol == IPPROTO_UDP
        && operation != SocketOperation::ConnectEx) {
        transport = SocketTransport::Udp;
        return true;
    }
    return false;
}

bool ExactEndpointMatch(
    const AllowedSocketEndpoint& allowed,
    const sockaddr* const address,
    const int length) noexcept {
    if (allowed.family == AF_INET
        && length == static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in requested{};
        std::memcpy(&requested, address, sizeof(requested));
        return requested.sin_family == AF_INET
            && requested.sin_port == allowed.port
            && std::memcmp(
                &requested.sin_addr,
                allowed.address.data(),
                sizeof(requested.sin_addr)) == 0;
    }
    if (allowed.family == AF_INET6
        && length == static_cast<int>(sizeof(sockaddr_in6))) {
        sockaddr_in6 requested{};
        std::memcpy(&requested, address, sizeof(requested));
        return requested.sin6_family == AF_INET6
            && requested.sin6_port == allowed.port
            && std::memcmp(
                &requested.sin6_addr,
                allowed.address.data(),
                sizeof(requested.sin6_addr)) == 0;
    }
    return false;
}

bool IsExplicitlyAdmitted(
    const HookContext& context,
    const SOCKET socketHandle,
    const SocketOperation operation,
    const sockaddr* const address,
    const int length) noexcept {
    if (context.denyOnly.load(std::memory_order_acquire)
        || EvaluateSocketOperation(operation, address, length)
            != NetworkDecision::AllowLoopback) {
        return false;
    }

    SocketTransport transport{};
    ADDRESS_FAMILY socketFamily = AF_UNSPEC;
    if (!ReadSocketIdentity(socketHandle, operation, transport, socketFamily)) {
        return false;
    }
    for (std::size_t index = 0;
         index < context.configuration.endpointCount;
         ++index) {
        const auto& allowed = context.configuration.endpoints[index];
        if (allowed.transport == transport
            && allowed.family == socketFamily
            && ExactEndpointMatch(allowed, address, length)) {
            return true;
        }
    }
    return false;
}

int DenySocketOperation(const SocketOperation operation) noexcept {
    ReportDenied(operation);
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}

BOOL DenyConnectEx() noexcept {
    ReportDenied(SocketOperation::ConnectEx);
    WSASetLastError(WSAEACCES);
    return FALSE;
}

int WSAAPI HookConnect(
    const SOCKET socketHandle,
    const sockaddr* const address,
    const int length) noexcept {
    CallbackLease callback;
    const auto& context = callback.Context();
    if (context == nullptr
        || !IsExplicitlyAdmitted(
            *context, socketHandle, SocketOperation::Connect, address, length)) {
        return DenySocketOperation(SocketOperation::Connect);
    }
    if (context->trampolines.connect == nullptr) {
        return DenySocketOperation(SocketOperation::Connect);
    }
    return context->trampolines.connect(socketHandle, address, length);
}

int WSAAPI HookWsaConnect(
    const SOCKET socketHandle,
    const sockaddr* const address,
    const int length,
    LPWSABUF callerData,
    LPWSABUF calleeData,
    LPQOS socketQos,
    LPQOS groupQos) noexcept {
    CallbackLease callback;
    const auto& context = callback.Context();
    if (context == nullptr
        || !IsExplicitlyAdmitted(
            *context, socketHandle, SocketOperation::WsaConnect, address, length)) {
        return DenySocketOperation(SocketOperation::WsaConnect);
    }
    if (context->trampolines.wsaConnect == nullptr) {
        return DenySocketOperation(SocketOperation::WsaConnect);
    }
    return context->trampolines.wsaConnect(
        socketHandle,
        address,
        length,
        callerData,
        calleeData,
        socketQos,
        groupQos);
}

int WSAAPI HookSendTo(
    const SOCKET socketHandle,
    const char* const buffer,
    const int bufferLength,
    const int flags,
    const sockaddr* const address,
    const int length) noexcept {
    CallbackLease callback;
    const auto& context = callback.Context();
    if (context == nullptr
        || !IsExplicitlyAdmitted(
            *context, socketHandle, SocketOperation::SendTo, address, length)) {
        return DenySocketOperation(SocketOperation::SendTo);
    }
    if (context->trampolines.sendTo == nullptr) {
        return DenySocketOperation(SocketOperation::SendTo);
    }
    return context->trampolines.sendTo(
        socketHandle, buffer, bufferLength, flags, address, length);
}

BOOL PASCAL HookConnectEx(
    const SOCKET socketHandle,
    const sockaddr* const address,
    const int length,
    PVOID sendBuffer,
    const DWORD sendDataLength,
    LPDWORD bytesSent,
    LPOVERLAPPED overlapped) noexcept {
    CallbackLease callback;
    const auto& context = callback.Context();
    if (context == nullptr
        || !IsExplicitlyAdmitted(
            *context, socketHandle, SocketOperation::ConnectEx, address, length)
        || context->trampolines.wsaIoctl == nullptr) {
        return DenyConnectEx();
    }

    GUID connectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX original = nullptr;
    DWORD returned = 0;
    if (context->trampolines.wsaIoctl(
            socketHandle,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            &original,
            sizeof(original),
            &returned,
            nullptr,
            nullptr) == SOCKET_ERROR
        || original == nullptr
        || original == &HookConnectEx
        || returned != sizeof(original)) {
        return DenyConnectEx();
    }

    return original(
        socketHandle,
        address,
        length,
        sendBuffer,
        sendDataLength,
        bytesSent,
        overlapped);
}

bool TryReadGuid(const LPVOID input, GUID& value) noexcept {
    if (input == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&value, input, sizeof(value));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RangesOverlap(
    const void* const first,
    const std::size_t firstLength,
    const void* const second,
    const std::size_t secondLength) noexcept {
    const auto firstStart = reinterpret_cast<std::uintptr_t>(first);
    const auto secondStart = reinterpret_cast<std::uintptr_t>(second);
    if (firstStart > (std::numeric_limits<std::uintptr_t>::max)() - firstLength
        || secondStart > (std::numeric_limits<std::uintptr_t>::max)() - secondLength) {
        return true;
    }
    return firstStart < secondStart + secondLength
        && secondStart < firstStart + firstLength;
}

bool ClearConnectExCallerOutput(
    const LPVOID output,
    const DWORD outputLength,
    const LPDWORD bytesReturned) noexcept {
    if (bytesReturned == nullptr) {
        return false;
    }
    __try {
        *bytesReturned = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (output == nullptr
        || outputLength < sizeof(LPFN_CONNECTEX)
        || RangesOverlap(
            output,
            sizeof(LPFN_CONNECTEX),
            bytesReturned,
            sizeof(*bytesReturned))) {
        return false;
    }
    __try {
        const LPFN_CONNECTEX empty = nullptr;
        std::memcpy(output, &empty, sizeof(empty));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PublishGuardedConnectEx(
    const LPVOID output,
    const LPDWORD bytesReturned) noexcept {
    __try {
        const LPFN_CONNECTEX guarded = &HookConnectEx;
        std::memcpy(output, &guarded, sizeof(guarded));
        *bytesReturned = sizeof(guarded);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int WSAAPI HookWsaIoctl(
    const SOCKET socketHandle,
    const DWORD controlCode,
    LPVOID input,
    const DWORD inputLength,
    LPVOID output,
    const DWORD outputLength,
    LPDWORD bytesReturned,
    LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutine) noexcept {
    CallbackLease callback;
    const auto& context = callback.Context();
    if (context == nullptr || context->trampolines.wsaIoctl == nullptr) {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }

    GUID requestedGuid{};
    bool connectExRequest = false;
    if (controlCode == SIO_GET_EXTENSION_FUNCTION_POINTER) {
        if (inputLength != sizeof(requestedGuid)
            || !TryReadGuid(input, requestedGuid)) {
            WSASetLastError(WSAEACCES);
            return SOCKET_ERROR;
        }
        const GUID connectExGuid = WSAID_CONNECTEX;
        connectExRequest = std::memcmp(
            &requestedGuid, &connectExGuid, sizeof(connectExGuid)) == 0;
    }
    if (!connectExRequest) {
        return context->trampolines.wsaIoctl(
            socketHandle,
            controlCode,
            input,
            inputLength,
            output,
            outputLength,
            bytesReturned,
            overlapped,
            completionRoutine);
    }

    if (!ClearConnectExCallerOutput(output, outputLength, bytesReturned)
        || context->denyOnly.load(std::memory_order_acquire)
        || overlapped != nullptr
        || completionRoutine != nullptr) {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }

    LPFN_CONNECTEX providerConnectEx = nullptr;
    DWORD providerBytes = 0;
    const int result = context->trampolines.wsaIoctl(
        socketHandle,
        controlCode,
        &requestedGuid,
        sizeof(requestedGuid),
        &providerConnectEx,
        sizeof(providerConnectEx),
        &providerBytes,
        nullptr,
        nullptr);
    if (result == SOCKET_ERROR) {
        return result;
    }

    if (providerConnectEx == nullptr
        || providerConnectEx == &HookConnectEx
        || providerBytes != sizeof(providerConnectEx)
        || !PublishGuardedConnectEx(output, bytesReturned)) {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }
    return 0;
}

bool ValidateConfiguration(const WinsockHookConfiguration& configuration) noexcept {
    if (configuration.endpointCount > configuration.endpoints.size()) {
        return false;
    }

    bool hasTcp = false;
    bool hasUdp = false;
    for (std::size_t index = 0; index < configuration.endpointCount; ++index) {
        const auto& endpoint = configuration.endpoints[index];
        bool* seen = nullptr;
        SocketOperation policyOperation{};
        switch (endpoint.transport) {
        case SocketTransport::Tcp:
            seen = &hasTcp;
            policyOperation = SocketOperation::Connect;
            break;
        case SocketTransport::Udp:
            seen = &hasUdp;
            policyOperation = SocketOperation::SendTo;
            break;
        default:
            return false;
        }
        if (*seen || endpoint.port == 0) {
            return false;
        }
        *seen = true;

        if (endpoint.family == AF_INET) {
            if (!std::all_of(
                    endpoint.address.begin() + sizeof(IN_ADDR),
                    endpoint.address.end(),
                    [](const std::uint8_t value) { return value == 0; })) {
                return false;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = endpoint.port;
            std::memcpy(&address.sin_addr, endpoint.address.data(), sizeof(address.sin_addr));
            if (EvaluateSocketOperation(
                    policyOperation,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) != NetworkDecision::AllowLoopback) {
                return false;
            }
        }
        else if (endpoint.family == AF_INET6) {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = endpoint.port;
            std::memcpy(&address.sin6_addr, endpoint.address.data(), sizeof(address.sin6_addr));
            if (EvaluateSocketOperation(
                    policyOperation,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) != NetworkDecision::AllowLoopback) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    return true;
}

struct HookDefinition {
    const wchar_t* module;
    const char* procedure;
    void* detour;
    void** original;
};

std::array<HookDefinition, 4> HookDefinitions(HookContext& context) noexcept {
    return {{
        {L"ws2_32.dll", "connect",
         reinterpret_cast<void*>(&HookConnect),
         reinterpret_cast<void**>(&context.trampolines.connect)},
        {L"ws2_32.dll", "WSAConnect",
         reinterpret_cast<void*>(&HookWsaConnect),
         reinterpret_cast<void**>(&context.trampolines.wsaConnect)},
        {L"ws2_32.dll", "sendto",
         reinterpret_cast<void*>(&HookSendTo),
         reinterpret_cast<void**>(&context.trampolines.sendTo)},
        {L"ws2_32.dll", "WSAIoctl",
         reinterpret_cast<void*>(&HookWsaIoctl),
         reinterpret_cast<void**>(&context.trampolines.wsaIoctl)},
    }};
}

class MinHookPlatform final : public HookPlatform {
public:
    bool Initialize() noexcept override {
        targetCount_ = 0;
        const auto status = MH_Initialize();
        ownsInitialization_ = status == MH_OK;
        return ownsInitialization_ || status == MH_ERROR_ALREADY_INITIALIZED;
    }

    void* ResolveTarget(
        const wchar_t* const moduleName,
        const char* const procedureName) noexcept override {
        auto module = GetModuleHandleW(moduleName);
        if (module == nullptr) {
            module = LoadLibraryW(moduleName);
        }
        return module == nullptr
            ? nullptr
            : reinterpret_cast<void*>(GetProcAddress(module, procedureName));
    }

    bool CreateHook(
        void* const target,
        void* const detour,
        void** const original) noexcept override {
        if (targetCount_ >= targets_.size()
            || MH_CreateHook(target, detour, original) != MH_OK) {
            return false;
        }
        targets_[targetCount_++] = target;
        return true;
    }

    bool QueueEnable(void* const target) noexcept override {
        return MH_QueueEnableHook(target) == MH_OK;
    }

    bool ApplyQueued() noexcept override { return MH_ApplyQueued() == MH_OK; }

    bool DisableAll() noexcept override {
        bool disabled = true;
        for (std::size_t index = 0; index < targetCount_; ++index) {
            const auto status = MH_DisableHook(targets_[index]);
            disabled = (status == MH_OK
                    || status == MH_ERROR_DISABLED
                    || status == MH_ERROR_NOT_CREATED)
                && disabled;
        }
        return disabled;
    }

    bool RemoveHook(void* const target) noexcept override {
        const auto status = MH_RemoveHook(target);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
            return false;
        }
        const auto found = std::find(
            targets_.begin(),
            targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
            target);
        if (found != targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_)) {
            std::move(
                found + 1,
                targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
                found);
            --targetCount_;
        }
        return true;
    }

    bool Uninitialize() noexcept override {
        if (!ownsInitialization_) {
            return targetCount_ == 0;
        }
        const auto status = MH_Uninitialize();
        if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED) {
            return false;
        }
        ownsInitialization_ = false;
        targetCount_ = 0;
        return true;
    }

private:
    bool ownsInitialization_ = false;
    std::array<void*, 4> targets_{};
    std::size_t targetCount_ = 0;
};

MinHookPlatform systemPlatform;

WinsockHookCleanupStatus CleanupLocked() noexcept {
    hooksInstalled.store(false, std::memory_order_release);
    if (lifecycle.context != nullptr) {
        lifecycle.context->denyOnly.store(true, std::memory_order_release);
    }
    if (lifecycle.platform == nullptr) {
        std::unique_lock callbackLock(callbackGate);
        activeContext.store({}, std::memory_order_release);
        lifecycle = {};
        return WinsockHookCleanupStatus::Success;
    }

    if (lifecycle.mayBeEnabled) {
        if (!lifecycle.platform->DisableAll()) {
            return WinsockHookCleanupStatus::Incomplete;
        }
        lifecycle.mayBeEnabled = false;
    }

    std::unique_lock callbackLock(callbackGate);
    bool allRemoved = true;
    for (std::size_t index = lifecycle.created.size(); index > 0; --index) {
        const auto slot = index - 1;
        if (!lifecycle.created[slot]) {
            continue;
        }
        if (lifecycle.platform->RemoveHook(lifecycle.targets[slot])) {
            lifecycle.created[slot] = false;
        }
        else {
            allRemoved = false;
        }
    }
    if (!allRemoved) {
        return WinsockHookCleanupStatus::Incomplete;
    }

    if (lifecycle.initialized) {
        if (!lifecycle.platform->Uninitialize()) {
            return WinsockHookCleanupStatus::Incomplete;
        }
        lifecycle.initialized = false;
    }

    activeContext.store({}, std::memory_order_release);
    lifecycle = {};
    return WinsockHookCleanupStatus::Success;
}

}  // namespace

WinsockHookInstallStatus InstallWinsockHooks(
    const WinsockHookConfiguration& configuration) noexcept {
    return InstallWinsockHooks(configuration, systemPlatform);
}

WinsockHookInstallStatus InstallWinsockHooks(
    const WinsockHookConfiguration& configuration,
    HookPlatform& platform) noexcept {
    std::scoped_lock lock(installMutex);
    if (lifecycle.platform != nullptr
        || activeContext.load(std::memory_order_acquire) != nullptr) {
        return WinsockHookInstallStatus::InstallFailed;
    }

    try {
        if (!ValidateConfiguration(configuration)) {
            return WinsockHookInstallStatus::InvalidConfiguration;
        }
        auto context = std::make_shared<HookContext>(configuration);
        if (!platform.Initialize()) {
            return WinsockHookInstallStatus::InstallFailed;
        }

        lifecycle.platform = &platform;
        lifecycle.context = context;
        lifecycle.initialized = true;
        activeContext.store(context, std::memory_order_release);
        const auto definitions = HookDefinitions(*context);

        for (std::size_t index = 0; index < definitions.size(); ++index) {
            lifecycle.targets[index] = platform.ResolveTarget(
                definitions[index].module,
                definitions[index].procedure);
            if (lifecycle.targets[index] == nullptr) {
                static_cast<void>(CleanupLocked());
                return WinsockHookInstallStatus::InstallFailed;
            }
        }
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            if (!platform.CreateHook(
                    lifecycle.targets[index],
                    definitions[index].detour,
                    definitions[index].original)) {
                static_cast<void>(CleanupLocked());
                return WinsockHookInstallStatus::InstallFailed;
            }
            lifecycle.created[index] = true;
        }
        for (const auto target : lifecycle.targets) {
            if (!platform.QueueEnable(target)) {
                static_cast<void>(CleanupLocked());
                return WinsockHookInstallStatus::InstallFailed;
            }
        }
        lifecycle.mayBeEnabled = true;
        if (!platform.ApplyQueued()) {
            static_cast<void>(CleanupLocked());
            return WinsockHookInstallStatus::InstallFailed;
        }

        hooksInstalled.store(true, std::memory_order_release);
        return WinsockHookInstallStatus::Success;
    }
    catch (...) {
        static_cast<void>(CleanupLocked());
        return WinsockHookInstallStatus::InstallFailed;
    }
}

WinsockHookCleanupStatus UninstallWinsockHooks() noexcept {
    std::scoped_lock lock(installMutex);
    return CleanupLocked();
}

bool WinsockHooksAreInstalled() noexcept {
    return hooksInstalled.load(std::memory_order_acquire);
}

WinsockAuditCounters CurrentWinsockAuditCounters() noexcept {
    const std::uint64_t connectCount = deniedCounters[0].load(std::memory_order_relaxed);
    const std::uint64_t wsaConnectCount = deniedCounters[1].load(std::memory_order_relaxed);
    const std::uint64_t sendToCount = deniedCounters[2].load(std::memory_order_relaxed);
    const std::uint64_t connectExCount = deniedCounters[3].load(std::memory_order_relaxed);
    return {
        connectCount,
        wsaConnectCount,
        sendToCount,
        connectExCount,
        connectCount + wsaConnectCount + sendToCount + connectExCount,
    };
}

}  // namespace DSRRandomizer::Network

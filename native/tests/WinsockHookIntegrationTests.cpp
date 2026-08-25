#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "hooks/MinHookCoordinator.h"
#include "network/WinsockHooks.h"

namespace {

using DSRRandomizer::Network::HookPlatform;
using DSRRandomizer::Network::WinsockHookCleanupStatus;
using DSRRandomizer::Network::WinsockHookConfiguration;
using DSRRandomizer::Network::WinsockHookInstallStatus;
using ConnectFunction = decltype(&connect);
using WsaConnectFunction = decltype(&WSAConnect);
using SendToFunction = decltype(&sendto);
using WsaIoctlFunction = decltype(&WSAIoctl);

struct HookFailures {
    std::size_t missingTarget = std::numeric_limits<std::size_t>::max();
    std::size_t failedCreate = std::numeric_limits<std::size_t>::max();
    std::size_t failedQueue = std::numeric_limits<std::size_t>::max();
    bool failApply = false;
};

class FixtureHookPlatform final : public HookPlatform {
public:
    explicit FixtureHookPlatform(const HookFailures failures = {})
        : failures_(failures) {}

    void BeginMutation() noexcept override {
        mutationLease_ = std::make_unique<
            DSRRandomizer::Hooks::MinHookMutationLease>();
    }

    void EndMutation() noexcept override {
        mutationLease_.reset();
        if (mutationReleasedEvent_ != nullptr) {
            SetEvent(mutationReleasedEvent_);
        }
    }

    bool Initialize() noexcept override {
        initialized_ = true;
        return true;
    }
    void* ResolveTarget(const wchar_t*, const char*) noexcept override {
        const auto index = resolveCount_++;
        return index == failures_.missingTarget
            ? nullptr
            : reinterpret_cast<void*>(0x20000ULL + (index * 0x100ULL));
    }
    bool CreateHook(void* target, void*, void** original) noexcept override {
        if (createCount_++ == failures_.failedCreate) {
            return false;
        }
        created_.insert(target);
        *original = target;
        return true;
    }
    bool QueueEnable(void* target) noexcept override {
        if (queueCount_++ == failures_.failedQueue) {
            return false;
        }
        queued_.insert(target);
        return true;
    }
    bool ApplyQueued() noexcept override {
        if (failures_.failApply) {
            enabled_.insert(queued_.begin(), queued_.end());
            return false;
        }
        enabled_.insert(queued_.begin(), queued_.end());
        return true;
    }
    bool DisableAll() noexcept override {
        enabled_.clear();
        return true;
    }
    bool RemoveHook(void* target) noexcept override {
        queued_.erase(target);
        enabled_.erase(target);
        created_.erase(target);
        return true;
    }
    bool Uninitialize() noexcept override {
        initialized_ = false;
        return true;
    }

    [[nodiscard]] bool WasRolledBack() const noexcept {
        return !initialized_ && created_.empty() && enabled_.empty();
    }
    [[nodiscard]] bool WasInitialized() const noexcept { return initialized_; }
    void SignalMutationRelease(const HANDLE event) noexcept {
        mutationReleasedEvent_ = event;
    }

private:
    HookFailures failures_;
    std::size_t resolveCount_ = 0;
    std::size_t createCount_ = 0;
    std::size_t queueCount_ = 0;
    bool initialized_ = false;
    std::set<void*> created_;
    std::set<void*> queued_;
    std::set<void*> enabled_;
    std::unique_ptr<DSRRandomizer::Hooks::MinHookMutationLease> mutationLease_;
    HANDLE mutationReleasedEvent_ = nullptr;
};

struct AdapterSpyState {
    std::uint64_t connectCalls = 0;
    std::uint64_t wsaConnectCalls = 0;
    std::uint64_t sendToCalls = 0;
    std::uint64_t wsaIoctlCalls = 0;
    std::uint64_t rawConnectExCalls = 0;
    void* lastProviderOutput = nullptr;
};

AdapterSpyState adapterSpy{};
std::atomic<std::uint64_t> completionRoutineCalls{0};

BOOL PASCAL RawConnectExSpy(
    SOCKET,
    const sockaddr*,
    int,
    PVOID,
    DWORD,
    LPDWORD,
    LPOVERLAPPED) {
    ++adapterSpy.rawConnectExCalls;
    return TRUE;
}

int WSAAPI OriginalConnectSpy(SOCKET, const sockaddr*, int) {
    ++adapterSpy.connectCalls;
    return 0;
}

int WSAAPI OriginalWsaConnectSpy(
    SOCKET,
    const sockaddr*,
    int,
    LPWSABUF,
    LPWSABUF,
    LPQOS,
    LPQOS) {
    ++adapterSpy.wsaConnectCalls;
    return 0;
}

int WSAAPI OriginalSendToSpy(
    SOCKET,
    const char*,
    int length,
    int,
    const sockaddr*,
    int) {
    ++adapterSpy.sendToCalls;
    return length;
}

int WSAAPI OriginalWsaIoctlSpy(
    SOCKET,
    DWORD controlCode,
    LPVOID,
    DWORD,
    LPVOID output,
    DWORD outputLength,
    LPDWORD bytesReturned,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    ++adapterSpy.wsaIoctlCalls;
    adapterSpy.lastProviderOutput = output;
    if (controlCode != SIO_GET_EXTENSION_FUNCTION_POINTER
        || output == nullptr
        || outputLength < sizeof(LPFN_CONNECTEX)
        || bytesReturned == nullptr) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const LPFN_CONNECTEX raw = &RawConnectExSpy;
    std::memcpy(output, &raw, sizeof(raw));
    *bytesReturned = sizeof(raw);
    return 0;
}

void CALLBACK CompletionRoutineSpy(
    DWORD,
    DWORD,
    LPWSAOVERLAPPED,
    DWORD) {
    completionRoutineCalls.fetch_add(1, std::memory_order_relaxed);
}

class AdapterHookPlatform final : public HookPlatform {
public:
    bool Initialize() noexcept override { return true; }

    void* ResolveTarget(const wchar_t*, const char* procedure) noexcept override {
        if (std::strcmp(procedure, "connect") == 0) {
            return reinterpret_cast<void*>(&OriginalConnectSpy);
        }
        if (std::strcmp(procedure, "WSAConnect") == 0) {
            return reinterpret_cast<void*>(&OriginalWsaConnectSpy);
        }
        if (std::strcmp(procedure, "sendto") == 0) {
            return reinterpret_cast<void*>(&OriginalSendToSpy);
        }
        if (std::strcmp(procedure, "WSAIoctl") == 0) {
            return reinterpret_cast<void*>(&OriginalWsaIoctlSpy);
        }
        return nullptr;
    }

    bool CreateHook(void* target, void* detour, void** original) noexcept override {
        *original = target;
        if (target == reinterpret_cast<void*>(&OriginalConnectSpy)) {
            connect = reinterpret_cast<ConnectFunction>(detour);
        }
        else if (target == reinterpret_cast<void*>(&OriginalWsaConnectSpy)) {
            wsaConnect = reinterpret_cast<WsaConnectFunction>(detour);
        }
        else if (target == reinterpret_cast<void*>(&OriginalSendToSpy)) {
            sendTo = reinterpret_cast<SendToFunction>(detour);
        }
        else if (target == reinterpret_cast<void*>(&OriginalWsaIoctlSpy)) {
            wsaIoctl = reinterpret_cast<WsaIoctlFunction>(detour);
        }
        else {
            return false;
        }
        return true;
    }

    bool QueueEnable(void*) noexcept override { return true; }
    bool ApplyQueued() noexcept override { return true; }
    bool DisableAll() noexcept override { return true; }
    bool RemoveHook(void*) noexcept override { return true; }
    bool Uninitialize() noexcept override { return true; }

    ConnectFunction connect = nullptr;
    WsaConnectFunction wsaConnect = nullptr;
    SendToFunction sendTo = nullptr;
    WsaIoctlFunction wsaIoctl = nullptr;
};

#pragma pack(push, 1)
struct AuthenticatedPayload {
    char operation[4];
    std::uint8_t nonce[DSRRandomizer::kProtectionNonceSize];
};
#pragma pack(pop)

class Socket final {
public:
    explicit Socket(const SOCKET value = INVALID_SOCKET) noexcept : value_(value) {}
    ~Socket() { Reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] SOCKET Get() const noexcept { return value_; }
    void Reset() noexcept {
        if (value_ != INVALID_SOCKET) {
            closesocket(value_);
            value_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET value_;
};

int Fail(const char* const message) {
    std::cerr << message << '\n';
    return 1;
}

int VerifyAtomicHookLifecycle() {
    WinsockHookConfiguration invalid{};
    invalid.endpointCount = invalid.endpoints.size() + 1;
    FixtureHookPlatform invalidPlatform;
    if (DSRRandomizer::Network::InstallWinsockHooks(invalid, invalidPlatform)
            != WinsockHookInstallStatus::InvalidConfiguration
        || invalidPlatform.WasInitialized()) {
        return Fail("out-of-bounds endpoint configuration was not rejected before hooks");
    }

    const WinsockHookConfiguration empty{};
    const HookFailures failures[] = {
        {.missingTarget = 2},
        {.failedCreate = 2},
        {.failedQueue = 2},
        {.failApply = true},
    };
    for (const auto& failure : failures) {
        FixtureHookPlatform platform(failure);
        if (DSRRandomizer::Network::InstallWinsockHooks(empty, platform)
                != WinsockHookInstallStatus::InstallFailed
            || !platform.WasRolledBack()
            || DSRRandomizer::Network::WinsockHooksAreInstalled()) {
            return Fail("partial Winsock hook installation did not roll back atomically");
        }
    }

    FixtureHookPlatform success;
    if (DSRRandomizer::Network::InstallWinsockHooks(empty, success)
            != WinsockHookInstallStatus::Success
        || !DSRRandomizer::Network::WinsockHooksAreInstalled()
        || DSRRandomizer::Network::UninstallWinsockHooks()
            != WinsockHookCleanupStatus::Success
        || !success.WasRolledBack()
        || DSRRandomizer::Network::WinsockHooksAreInstalled()) {
        return Fail("successful Winsock hook group did not cleanly uninstall");
    }

    FixtureHookPlatform mutationBarrier;
    if (DSRRandomizer::Network::InstallWinsockHooks(empty, mutationBarrier)
        != WinsockHookInstallStatus::Success) {
        return Fail("Winsock mutation barrier setup did not install hooks");
    }
    const HANDLE callbackEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE allowMutation = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE mutationAcquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE callbackRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE cleanupMutationReleased =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (callbackEntered == nullptr || allowMutation == nullptr
        || mutationAcquired == nullptr || callbackRelease == nullptr
        || cleanupMutationReleased == nullptr) {
        ExitProcess(98);
    }
    mutationBarrier.SignalMutationRelease(cleanupMutationReleased);
    std::thread callback([&]() {
        DSRRandomizer::Network::Testing::
            HoldWinsockHookCallbackWhileWaitingForMutation(
                callbackEntered,
                allowMutation,
                mutationAcquired,
                callbackRelease);
    });
    if (WaitForSingleObject(callbackEntered, 5000) != WAIT_OBJECT_0) {
        ExitProcess(99);
    }
    WinsockHookCleanupStatus cleanupStatus =
        WinsockHookCleanupStatus::Incomplete;
    std::thread cleanup([&]() {
        cleanupStatus = DSRRandomizer::Network::UninstallWinsockHooks();
    });
    if (WaitForSingleObject(cleanupMutationReleased, 5000)
        != WAIT_OBJECT_0) {
        ExitProcess(100);
    }
    SetEvent(allowMutation);
    if (WaitForSingleObject(mutationAcquired, 5000) != WAIT_OBJECT_0) {
        ExitProcess(101);
    }
    SetEvent(callbackRelease);
    callback.join();
    cleanup.join();
    for (const auto event : {
             callbackEntered,
             allowMutation,
             mutationAcquired,
             callbackRelease,
             cleanupMutationReleased}) {
        CloseHandle(event);
    }
    if (cleanupStatus != WinsockHookCleanupStatus::Success
        || !mutationBarrier.WasRolledBack()) {
        return Fail("Winsock cleanup held mutation ownership while draining callbacks");
    }
    return 0;
}

sockaddr_in IPv4(const std::array<unsigned char, 4>& address, unsigned short port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    std::memcpy(&endpoint.sin_addr, address.data(), address.size());
    return endpoint;
}

int AdapterFailure(const char* message) {
    static_cast<void>(DSRRandomizer::Network::UninstallWinsockHooks());
    return Fail(message);
}

int VerifyAdapterSecurityBranches() {
    WinsockHookConfiguration configuration{};
    configuration.endpointCount = 2;
    auto& allowed = configuration.endpoints[0];
    allowed.transport = DSRRandomizer::SocketTransport::Tcp;
    allowed.family = AF_INET;
    allowed.port = htons(42000);
    allowed.address[0] = 127;
    allowed.address[1] = 0;
    allowed.address[2] = 0;
    allowed.address[3] = 1;
    auto& allowedUdp = configuration.endpoints[1];
    allowedUdp.transport = DSRRandomizer::SocketTransport::Udp;
    allowedUdp.family = AF_INET;
    allowedUdp.port = htons(42002);
    allowedUdp.address[0] = 127;
    allowedUdp.address[1] = 0;
    allowedUdp.address[2] = 0;
    allowedUdp.address[3] = 1;

    AdapterHookPlatform platform;
    adapterSpy = {};
    if (DSRRandomizer::Network::InstallWinsockHooks(configuration, platform)
            != WinsockHookInstallStatus::Success
        || platform.connect == nullptr
        || platform.wsaConnect == nullptr
        || platform.sendTo == nullptr
        || platform.wsaIoctl == nullptr) {
        return AdapterFailure("unable to install adapter spy hook group");
    }

    Socket stream(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Socket datagram(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    const auto allowedEndpoint = IPv4({127, 0, 0, 1}, 42000);
    const auto allowedUdpEndpoint = IPv4({127, 0, 0, 1}, 42002);
    const auto deniedEndpoint = IPv4({127, 0, 0, 1}, 42001);
    if (stream.Get() == INVALID_SOCKET || datagram.Get() == INVALID_SOCKET) {
        return AdapterFailure("unable to create adapter spy sockets");
    }

    if (platform.connect(
            stream.Get(),
            reinterpret_cast<const sockaddr*>(&allowedEndpoint),
            sizeof(allowedEndpoint)) != 0
        || adapterSpy.connectCalls != 1) {
        return AdapterFailure("authorized stream connect did not call its original once");
    }
    adapterSpy.connectCalls = 0;

    WSASetLastError(0);
    if (platform.connect(
            datagram.Get(),
            reinterpret_cast<const sockaddr*>(&allowedEndpoint),
            sizeof(allowedEndpoint)) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.connectCalls != 0) {
        return AdapterFailure("UDP connect used a TCP-only endpoint or called its original");
    }
    WSASetLastError(0);
    if (platform.wsaConnect(
            datagram.Get(),
            reinterpret_cast<const sockaddr*>(&allowedEndpoint),
            sizeof(allowedEndpoint),
            nullptr,
            nullptr,
            nullptr,
            nullptr) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.wsaConnectCalls != 0) {
        return AdapterFailure("UDP WSAConnect used a TCP-only endpoint or called its original");
    }
    if (platform.connect(
            datagram.Get(),
            reinterpret_cast<const sockaddr*>(&allowedUdpEndpoint),
            sizeof(allowedUdpEndpoint)) != 0
        || adapterSpy.connectCalls != 1) {
        return AdapterFailure("configured UDP connect did not call its original once");
    }
    adapterSpy.connectCalls = 0;

    WSASetLastError(0);
    if (platform.connect(
            stream.Get(),
            reinterpret_cast<const sockaddr*>(&deniedEndpoint),
            sizeof(deniedEndpoint)) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.connectCalls != 0) {
        return AdapterFailure("denied connect called its original trampoline");
    }
    WSASetLastError(0);
    if (platform.wsaConnect(
            stream.Get(),
            reinterpret_cast<const sockaddr*>(&deniedEndpoint),
            sizeof(deniedEndpoint),
            nullptr,
            nullptr,
            nullptr,
            nullptr) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.wsaConnectCalls != 0) {
        return AdapterFailure("denied WSAConnect called its original trampoline");
    }
    constexpr char marker[] = "denied";
    WSASetLastError(0);
    if (platform.sendTo(
            datagram.Get(),
            marker,
            sizeof(marker),
            0,
            reinterpret_cast<const sockaddr*>(&deniedEndpoint),
            sizeof(deniedEndpoint)) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.sendToCalls != 0) {
        return AdapterFailure("denied sendto called its original trampoline");
    }

    GUID connectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX callerPointer = nullptr;
    DWORD callerBytes = 0;
    if (platform.wsaIoctl(
            stream.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            &callerPointer,
            sizeof(callerPointer),
            &callerBytes,
            nullptr,
            nullptr) == SOCKET_ERROR
        || callerPointer == nullptr
        || callerPointer == &RawConnectExSpy
        || callerBytes != sizeof(callerPointer)
        || adapterSpy.wsaIoctlCalls != 1
        || adapterSpy.lastProviderOutput == &callerPointer) {
        return AdapterFailure("ConnectEx provider pointer reached caller-owned output");
    }
    OVERLAPPED deniedConnectEx{};
    WSASetLastError(0);
    if (callerPointer(
            stream.Get(),
            reinterpret_cast<const sockaddr*>(&deniedEndpoint),
            sizeof(deniedEndpoint),
            nullptr,
            0,
            nullptr,
            &deniedConnectEx)
        || WSAGetLastError() != WSAEACCES
        || adapterSpy.rawConnectExCalls != 0
        || adapterSpy.wsaIoctlCalls != 1) {
        return AdapterFailure("denied guarded ConnectEx invoked the raw provider pointer");
    }

    const auto providerCalls = adapterSpy.wsaIoctlCalls;
    callerPointer = reinterpret_cast<LPFN_CONNECTEX>(1);
    callerBytes = 99;
    WSASetLastError(0);
    if (platform.wsaIoctl(
            stream.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            &callerPointer,
            sizeof(callerPointer),
            &callerBytes,
            nullptr,
            &CompletionRoutineSpy) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || callerPointer != nullptr
        || callerBytes != 0
        || adapterSpy.wsaIoctlCalls != providerCalls
        || completionRoutineCalls.load(std::memory_order_relaxed) != 0) {
        return AdapterFailure("completion-routine ConnectEx retrieval did not fail closed");
    }

    OVERLAPPED overlapped{};
    callerPointer = reinterpret_cast<LPFN_CONNECTEX>(1);
    callerBytes = 99;
    WSASetLastError(0);
    if (platform.wsaIoctl(
            stream.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            &callerPointer,
            sizeof(callerPointer),
            &callerBytes,
            &overlapped,
            nullptr) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || callerPointer != nullptr
        || callerBytes != 0
        || adapterSpy.wsaIoctlCalls != providerCalls) {
        return AdapterFailure("overlapped ConnectEx retrieval did not fail closed");
    }

    callerBytes = 99;
    WSASetLastError(0);
    if (platform.wsaIoctl(
            stream.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            nullptr,
            sizeof(LPFN_CONNECTEX),
            &callerBytes,
            nullptr,
            nullptr) != SOCKET_ERROR
        || WSAGetLastError() != WSAEACCES
        || callerBytes != 0
        || adapterSpy.wsaIoctlCalls != providerCalls) {
        return AdapterFailure("invalid ConnectEx output buffer reached provider trampoline");
    }

    if (DSRRandomizer::Network::UninstallWinsockHooks()
            != WinsockHookCleanupStatus::Success) {
        return Fail("adapter spy hook group did not uninstall");
    }
    return 0;
}

std::wstring Quote(const std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize> CreateNonce() {
    std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize> nonce{};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        const auto mixed = stamp ^ (pid << (index % 17))
            ^ (0x9e3779b97f4a7c15ULL * (index + 1));
        nonce[index] = static_cast<std::uint8_t>(mixed >> ((index % 8) * 8));
    }
    return nonce;
}

std::wstring HexNonce(
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(nonce.size() * 2);
    for (const auto value : nonce) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

bool WaitReadable(const SOCKET socketHandle, const long milliseconds) {
    fd_set readSet{};
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return select(0, &readSet, nullptr, nullptr, &timeout) == 1;
}

bool BindLoopback(Socket& socketHandle, unsigned short& port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = 0;
    if (bind(
            socketHandle.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR) {
        return false;
    }
    int length = sizeof(endpoint);
    if (getsockname(
            socketHandle.Get(),
            reinterpret_cast<sockaddr*>(&endpoint),
            &length) == SOCKET_ERROR
        || length != sizeof(endpoint)) {
        return false;
    }
    port = ntohs(endpoint.sin_port);
    return port != 0;
}

bool BindLoopbackPort(Socket& socketHandle, const unsigned short port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = htons(port);
    return bind(
        socketHandle.Get(),
        reinterpret_cast<const sockaddr*>(&endpoint),
        sizeof(endpoint)) != SOCKET_ERROR;
}

bool PayloadMatches(
    const AuthenticatedPayload& payload,
    const std::array<char, 4>& operation,
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    return std::equal(operation.begin(), operation.end(), payload.operation)
        && std::equal(nonce.begin(), nonce.end(), payload.nonce);
}

bool ReceiveAuthenticatedTcp(
    const Socket& listener,
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    if (!WaitReadable(listener.Get(), 5000)) {
        return false;
    }
    Socket client(accept(listener.Get(), nullptr, nullptr));
    if (client.Get() == INVALID_SOCKET || !WaitReadable(client.Get(), 5000)) {
        return false;
    }
    AuthenticatedPayload payload{};
    int received = 0;
    while (received < static_cast<int>(sizeof(payload))) {
        const int current = recv(
            client.Get(),
            reinterpret_cast<char*>(&payload) + received,
            static_cast<int>(sizeof(payload)) - received,
            0);
        if (current <= 0) {
            return false;
        }
        received += current;
    }
    return PayloadMatches(payload, {'T', 'C', 'P', '1'}, nonce)
        && !WaitReadable(listener.Get(), 100);
}

bool ReceiveAuthenticatedUdp(
    const Socket& listener,
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    if (!WaitReadable(listener.Get(), 5000)) {
        return false;
    }
    AuthenticatedPayload payload{};
    sockaddr_in sender{};
    int senderLength = sizeof(sender);
    const int received = recvfrom(
        listener.Get(),
        reinterpret_cast<char*>(&payload),
        sizeof(payload),
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &senderLength);
    return received == sizeof(payload)
        && senderLength == sizeof(sender)
        && sender.sin_family == AF_INET
        && (ntohl(sender.sin_addr.s_addr) >> 24) == 127
        && PayloadMatches(payload, {'U', 'D', 'P', '1'}, nonce)
        && !WaitReadable(listener.Get(), 100);
}

bool ReadHandshake(
    const HANDLE pipe,
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    DSRRandomizer::ProtectionHandshakeMessage message{};
    DWORD read = 0;
    const auto requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Winsock);
    return ReadFile(
            pipe,
            &message,
            static_cast<DWORD>(sizeof(message)),
            &read,
            nullptr)
        && read == sizeof(message)
        && message.magic == DSRRandomizer::kProtectionMagic
        && message.version == DSRRandomizer::kProtectionProtocolVersion
        && message.size == sizeof(message)
        && std::equal(nonce.begin(), nonce.end(), message.nonce)
        && message.status == 0
        && message.activeFlags == requiredFlags;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        return Fail("expected network fixture and guard paths");
    }
    if (const auto lifecycleResult = VerifyAtomicHookLifecycle(); lifecycleResult != 0) {
        return lifecycleResult;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return Fail("WSAStartup failed");
    }
    if (const auto adapterResult = VerifyAdapterSecurityBranches(); adapterResult != 0) {
        WSACleanup();
        return adapterResult;
    }
    Socket tcpListener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Socket udpListener(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    Socket deniedTcpListener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Socket wrongTransportUdpListener(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    unsigned short tcpPort = 0;
    unsigned short udpPort = 0;
    unsigned short deniedTcpPort = 0;
    if (tcpListener.Get() == INVALID_SOCKET
        || udpListener.Get() == INVALID_SOCKET
        || deniedTcpListener.Get() == INVALID_SOCKET
        || wrongTransportUdpListener.Get() == INVALID_SOCKET
        || !BindLoopback(tcpListener, tcpPort)
        || listen(tcpListener.Get(), 1) == SOCKET_ERROR
        || !BindLoopback(udpListener, udpPort)
        || !BindLoopback(deniedTcpListener, deniedTcpPort)
        || listen(deniedTcpListener.Get(), 1) == SOCKET_ERROR
        || !BindLoopbackPort(wrongTransportUdpListener, tcpPort)) {
        WSACleanup();
        return Fail("unable to create loopback listeners");
    }

    const auto nonce = CreateNonce();
    const auto pipeName = L"\\\\.\\pipe\\DSRRandomizer-WinsockHook-"
        + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        static_cast<DWORD>(sizeof(DSRRandomizer::ProtectionHandshakeMessage)),
        static_cast<DWORD>(sizeof(DSRRandomizer::ProtectionHandshakeMessage)),
        5000,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        WSACleanup();
        return Fail("unable to create handshake pipe");
    }

    const HANDLE connectedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED connection{};
    connection.hEvent = connectedEvent;
    const BOOL connectedImmediately = ConnectNamedPipe(pipe, &connection);
    const auto connectError = connectedImmediately ? ERROR_SUCCESS : GetLastError();
    if (connectedEvent == nullptr
        || (!connectedImmediately
            && connectError != ERROR_IO_PENDING
            && connectError != ERROR_PIPE_CONNECTED)) {
        if (connectedEvent != nullptr) {
            CloseHandle(connectedEvent);
        }
        CloseHandle(pipe);
        WSACleanup();
        return Fail("unable to await handshake pipe connection");
    }
    if (connectError == ERROR_PIPE_CONNECTED) {
        SetEvent(connectedEvent);
    }

    auto commandLine = Quote(argv[1])
        + L" " + Quote(argv[2])
        + L" " + Quote(pipeName)
        + L" " + std::to_wstring(tcpPort)
        + L" " + std::to_wstring(udpPort)
        + L" " + std::to_wstring(deniedTcpPort)
        + L" " + HexNonce(nonce);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            argv[1],
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        CancelIoEx(pipe, &connection);
        CloseHandle(connectedEvent);
        CloseHandle(pipe);
        WSACleanup();
        return Fail("unable to start network fixture");
    }
    CloseHandle(process.hThread);

    const HANDLE connectionOrExit[] = {connectedEvent, process.hProcess};
    const auto connectionWait = WaitForMultipleObjects(2, connectionOrExit, FALSE, 10000);
    bool handshakeValid = false;
    if (connectionWait == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        handshakeValid = (connectedImmediately
                || connectError == ERROR_PIPE_CONNECTED
                || GetOverlappedResult(pipe, &connection, &transferred, FALSE))
            && ReadHandshake(pipe, nonce);
    }
    else {
        CancelIoEx(pipe, &connection);
    }
    CloseHandle(connectedEvent);
    CloseHandle(pipe);

    const bool tcpAuthenticated = ReceiveAuthenticatedTcp(tcpListener, nonce);
    const bool udpAuthenticated = ReceiveAuthenticatedUdp(udpListener, nonce);
    const auto waitResult = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
    }
    const bool deniedListenersSilent = !WaitReadable(deniedTcpListener.Get(), 100)
        && !WaitReadable(wrongTransportUdpListener.Get(), 100);
    CloseHandle(process.hProcess);
    tcpListener.Reset();
    udpListener.Reset();
    deniedTcpListener.Reset();
    wrongTransportUdpListener.Reset();
    WSACleanup();

    if (!handshakeValid) {
        std::cerr << "network fixture exit code before/without handshake: "
                  << exitCode << '\n';
        return Fail("network fixture handshake was invalid");
    }
    if (!tcpAuthenticated || !udpAuthenticated) {
        return Fail("listener received missing or unauthenticated loopback traffic");
    }
    if (!deniedListenersSilent) {
        return Fail("a denied adapter emitted traffic to a controlled listener");
    }
    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        std::cerr << "network fixture exit code: " << exitCode << '\n';
        return Fail("network fixture did not deny every external socket attempt");
    }
    return 0;
}

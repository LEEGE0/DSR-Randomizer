#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "network/WinsockHooks.h"

namespace {

using DSRRandomizer::Network::HookPlatform;
using DSRRandomizer::Network::WinsockHookCleanupStatus;
using DSRRandomizer::Network::WinsockHookConfiguration;
using DSRRandomizer::Network::WinsockHookInstallStatus;

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

private:
    HookFailures failures_;
    std::size_t resolveCount_ = 0;
    std::size_t createCount_ = 0;
    std::size_t queueCount_ = 0;
    bool initialized_ = false;
    std::set<void*> created_;
    std::set<void*> queued_;
    std::set<void*> enabled_;
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
    Socket tcpListener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Socket udpListener(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    unsigned short tcpPort = 0;
    unsigned short udpPort = 0;
    if (tcpListener.Get() == INVALID_SOCKET
        || udpListener.Get() == INVALID_SOCKET
        || !BindLoopback(tcpListener, tcpPort)
        || listen(tcpListener.Get(), 1) == SOCKET_ERROR
        || !BindLoopback(udpListener, udpPort)) {
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
    CloseHandle(process.hProcess);
    tcpListener.Reset();
    udpListener.Reset();
    WSACleanup();

    if (!handshakeValid) {
        std::cerr << "network fixture exit code before/without handshake: "
                  << exitCode << '\n';
        return Fail("network fixture handshake was invalid");
    }
    if (!tcpAuthenticated || !udpAuthenticated) {
        return Fail("listener received missing or unauthenticated loopback traffic");
    }
    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        std::cerr << "network fixture exit code: " << exitCode << '\n';
        return Fail("network fixture did not deny every external socket attempt");
    }
    return 0;
}

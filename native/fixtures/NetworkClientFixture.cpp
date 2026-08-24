#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "network/WinsockHooks.h"

namespace {

using InitializeProtectionFunction = std::uint32_t(__stdcall*)(
    DSRRandomizer::ProtectionInitBlock*);
using QueryWinsockAuditCountersFunction = std::uint32_t(__stdcall*)(
    DSRRandomizer::Network::WinsockAuditCounters*, std::uint32_t);

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

bool ParsePort(const wchar_t* const text, unsigned short& port) noexcept {
    wchar_t* end = nullptr;
    const auto parsed = wcstoul(text, &end, 10);
    if (text == end || end == nullptr || *end != L'\0' || parsed == 0
        || parsed > 65535) {
        return false;
    }
    port = static_cast<unsigned short>(parsed);
    return true;
}

int HexDigit(const wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    if (value >= L'a' && value <= L'f') {
        return 10 + value - L'a';
    }
    if (value >= L'A' && value <= L'F') {
        return 10 + value - L'A';
    }
    return -1;
}

bool ParseNonce(
    const wchar_t* const text,
    std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) noexcept {
    if (wcsnlen_s(text, (nonce.size() * 2) + 1) != nonce.size() * 2) {
        return false;
    }
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        const int high = HexDigit(text[index * 2]);
        const int low = HexDigit(text[(index * 2) + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        nonce[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

void ConfigureIPv4Endpoint(
    DSRRandomizer::ProtectionSocketEndpoint& endpoint,
    const DSRRandomizer::SocketTransport transport,
    const unsigned short port) noexcept {
    endpoint.transport = static_cast<std::uint16_t>(transport);
    endpoint.family = AF_INET;
    endpoint.port = htons(port);
    endpoint.reserved = 0;
    endpoint.address[0] = 127;
    endpoint.address[1] = 0;
    endpoint.address[2] = 0;
    endpoint.address[3] = 1;
}

sockaddr_in IPv4Endpoint(
    const std::array<unsigned char, 4>& address,
    const unsigned short port) noexcept {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    std::memcpy(&endpoint.sin_addr, address.data(), address.size());
    return endpoint;
}

bool ExpectDenied(const int result) noexcept {
    return result == SOCKET_ERROR && WSAGetLastError() == WSAEACCES;
}

bool SendAuthenticatedTraffic(
    const unsigned short tcpPort,
    const unsigned short udpPort,
    const std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize>& nonce) {
    AuthenticatedPayload tcpPayload{{'T', 'C', 'P', '1'}, {}};
    AuthenticatedPayload udpPayload{{'U', 'D', 'P', '1'}, {}};
    std::copy(nonce.begin(), nonce.end(), tcpPayload.nonce);
    std::copy(nonce.begin(), nonce.end(), udpPayload.nonce);

    const auto tcpEndpoint = IPv4Endpoint({127, 0, 0, 1}, tcpPort);
    Socket tcp(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (tcp.Get() == INVALID_SOCKET
        || connect(
            tcp.Get(),
            reinterpret_cast<const sockaddr*>(&tcpEndpoint),
            sizeof(tcpEndpoint)) == SOCKET_ERROR
        || send(
            tcp.Get(),
            reinterpret_cast<const char*>(&tcpPayload),
            sizeof(tcpPayload),
            0) != sizeof(tcpPayload)) {
        return false;
    }

    const auto udpEndpoint = IPv4Endpoint({127, 0, 0, 1}, udpPort);
    Socket udp(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    return udp.Get() != INVALID_SOCKET
        && sendto(
            udp.Get(),
            reinterpret_cast<const char*>(&udpPayload),
            sizeof(udpPayload),
            0,
            reinterpret_cast<const sockaddr*>(&udpEndpoint),
            sizeof(udpEndpoint)) == sizeof(udpPayload);
}

bool VerifyConnectExDenied(const sockaddr_in& externalEndpoint) {
    Socket socketHandle(WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED));
    if (socketHandle.Get() == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (bind(
            socketHandle.Get(),
            reinterpret_cast<const sockaddr*>(&local),
            sizeof(local)) == SOCKET_ERROR) {
        return false;
    }

    GUID connectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX connectEx = nullptr;
    DWORD bytes = 0;
    if (WSAIoctl(
            socketHandle.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connectExGuid,
            sizeof(connectExGuid),
            &connectEx,
            sizeof(connectEx),
            &bytes,
            nullptr,
            nullptr) == SOCKET_ERROR
        || connectEx == nullptr
        || bytes != sizeof(connectEx)) {
        return false;
    }

    OVERLAPPED overlapped{};
    WSASetLastError(0);
    return !connectEx(
            socketHandle.Get(),
            reinterpret_cast<const sockaddr*>(&externalEndpoint),
            sizeof(externalEndpoint),
            nullptr,
            0,
            nullptr,
            &overlapped)
        && WSAGetLastError() == WSAEACCES;
}

bool VerifyUnauthorizedTrafficDenied(const unsigned short admittedTcpPort) {
    const auto externalEndpoint = IPv4Endpoint({192, 0, 2, 1}, 9);

    const auto unadmittedLoopback = IPv4Endpoint({127, 0, 0, 2}, admittedTcpPort);
    Socket loopbackSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    WSASetLastError(0);
    if (loopbackSocket.Get() == INVALID_SOCKET
        || !ExpectDenied(connect(
            loopbackSocket.Get(),
            reinterpret_cast<const sockaddr*>(&unadmittedLoopback),
            sizeof(unadmittedLoopback)))) {
        return false;
    }

    Socket connectSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    WSASetLastError(0);
    if (connectSocket.Get() == INVALID_SOCKET
        || !ExpectDenied(connect(
            connectSocket.Get(),
            reinterpret_cast<const sockaddr*>(&externalEndpoint),
            sizeof(externalEndpoint)))) {
        return false;
    }

    Socket wsaConnectSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    WSASetLastError(0);
    if (wsaConnectSocket.Get() == INVALID_SOCKET
        || !ExpectDenied(WSAConnect(
            wsaConnectSocket.Get(),
            reinterpret_cast<const sockaddr*>(&externalEndpoint),
            sizeof(externalEndpoint),
            nullptr,
            nullptr,
            nullptr,
            nullptr))) {
        return false;
    }

    Socket udpSocket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    constexpr char marker[] = "denied";
    WSASetLastError(0);
    if (udpSocket.Get() == INVALID_SOCKET
        || !ExpectDenied(sendto(
            udpSocket.Get(),
            marker,
            static_cast<int>(sizeof(marker)),
            0,
            reinterpret_cast<const sockaddr*>(&externalEndpoint),
            sizeof(externalEndpoint)))) {
        return false;
    }

    return VerifyConnectExDenied(externalEndpoint);
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 6) {
        return 2;
    }

    unsigned short tcpPort = 0;
    unsigned short udpPort = 0;
    std::array<std::uint8_t, DSRRandomizer::kProtectionNonceSize> nonce{};
    if (!ParsePort(argv[3], tcpPort)
        || !ParsePort(argv[4], udpPort)
        || !ParseNonce(argv[5], nonce)) {
        return 3;
    }

    const HMODULE guard = LoadLibraryW(argv[1]);
    if (guard == nullptr) {
        return 4;
    }
    const auto initialize = reinterpret_cast<InitializeProtectionFunction>(
        GetProcAddress(guard, "InitializeProtection"));
    const auto queryCounters = reinterpret_cast<QueryWinsockAuditCountersFunction>(
        GetProcAddress(guard, "QueryWinsockAuditCounters"));
    if (initialize == nullptr || queryCounters == nullptr) {
        return 5;
    }

    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Winsock);
    block.socketEndpointCount = 2;
    ConfigureIPv4Endpoint(
        block.socketEndpoints[0], DSRRandomizer::SocketTransport::Tcp, tcpPort);
    ConfigureIPv4Endpoint(
        block.socketEndpoints[1], DSRRandomizer::SocketTransport::Udp, udpPort);
    std::copy(nonce.begin(), nonce.end(), block.nonce);
    if (wcscpy_s(
            block.pipeName,
            DSRRandomizer::kProtectionPipeNameCharacters,
            argv[2]) != 0) {
        return 6;
    }

    if (initialize(&block) != 0) {
        return 7;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return 8;
    }
    const bool localSucceeded = SendAuthenticatedTraffic(tcpPort, udpPort, nonce);
    const bool unauthorizedWasDenied = VerifyUnauthorizedTrafficDenied(tcpPort);
    DSRRandomizer::Network::WinsockAuditCounters counters{};
    const auto counterStatus = queryCounters(&counters, sizeof(counters));
    WSACleanup();

    if (!localSucceeded) {
        return 9;
    }
    if (!unauthorizedWasDenied) {
        return 10;
    }
    if (counterStatus != ERROR_SUCCESS
        || counters.connect != 2
        || counters.wsaConnect != 1
        || counters.sendTo != 1
        || counters.connectEx != 1
        || counters.total != 5) {
        return 11;
    }
    return 0;
}

#include <array>
#include <cstring>
#include <iostream>

#include <winsock2.h>
#include <ws2ipdef.h>

#include "network/NetworkPolicy.h"

namespace {

using DSRRandomizer::Network::EvaluateSocketOperation;
using DSRRandomizer::Network::NetworkDecision;
using DSRRandomizer::Network::SocketOperation;

sockaddr_in IPv4(const std::array<unsigned char, 4>& address, const unsigned short port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    std::memcpy(&endpoint.sin_addr, address.data(), address.size());
    return endpoint;
}

sockaddr_in6 IPv6(const std::array<unsigned char, 16>& address, const unsigned short port) {
    sockaddr_in6 endpoint{};
    endpoint.sin6_family = AF_INET6;
    endpoint.sin6_port = htons(port);
    std::memcpy(&endpoint.sin6_addr, address.data(), address.size());
    return endpoint;
}

int Fail(const char* const name, const NetworkDecision actual, const NetworkDecision expected) {
    std::cerr << "case failed: " << name << "\nexpected: "
              << static_cast<int>(expected) << "\nactual: "
              << static_cast<int>(actual) << '\n';
    return 1;
}

int RequireDecision(
    const char* const name,
    const SocketOperation operation,
    const sockaddr* const address,
    const int length,
    const NetworkDecision expected) {
    const auto actual = EvaluateSocketOperation(operation, address, length);
    return actual == expected ? 0 : Fail(name, actual, expected);
}

}  // namespace

int main() {
    const auto ipv4Loopback = IPv4({127, 0, 0, 1}, 42000);
    const auto ipv4LoopbackLastAddress = IPv4({127, 255, 255, 255}, 42000);
    const auto ipv4DnsResult = IPv4({8, 8, 8, 8}, 53);
    const auto ipv4Broadcast = IPv4({255, 255, 255, 255}, 42000);
    const auto ipv4Multicast = IPv4({224, 0, 0, 1}, 42000);
    const auto ipv6Loopback = IPv6({
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 42000);
    const auto ipv6DnsResult = IPv6({
        0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0,
        0, 0, 0, 0, 0, 0, 0x88, 0x88}, 53);
    const auto ipv6Multicast = IPv6({
        0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 42000);
    const auto ipv4MappedLoopback = IPv6({
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1}, 42000);
    const auto ipv4MappedDnsResult = IPv6({
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 8, 8}, 53);

    const struct Case {
        const char* name;
        SocketOperation operation;
        const sockaddr* address;
        int length;
        NetworkDecision expected;
    } cases[] = {
        {"connect allows IPv4 loopback", SocketOperation::Connect,
         reinterpret_cast<const sockaddr*>(&ipv4Loopback), sizeof(ipv4Loopback),
         NetworkDecision::AllowLoopback},
        {"connect allows the final IPv4 loopback address", SocketOperation::Connect,
         reinterpret_cast<const sockaddr*>(&ipv4LoopbackLastAddress), sizeof(ipv4LoopbackLastAddress),
         NetworkDecision::AllowLoopback},
        {"connect denies public IPv4 DNS result", SocketOperation::Connect,
         reinterpret_cast<const sockaddr*>(&ipv4DnsResult), sizeof(ipv4DnsResult),
         NetworkDecision::DenyNonLoopback},
        {"sendto allows IPv6 loopback", SocketOperation::SendTo,
         reinterpret_cast<const sockaddr*>(&ipv6Loopback), sizeof(ipv6Loopback),
         NetworkDecision::AllowLoopback},
        {"sendto denies public IPv6 DNS result", SocketOperation::SendTo,
         reinterpret_cast<const sockaddr*>(&ipv6DnsResult), sizeof(ipv6DnsResult),
         NetworkDecision::DenyNonLoopback},
        {"WSAConnect allows IPv4-mapped IPv6 loopback", SocketOperation::WsaConnect,
         reinterpret_cast<const sockaddr*>(&ipv4MappedLoopback), sizeof(ipv4MappedLoopback),
         NetworkDecision::AllowLoopback},
        {"ConnectEx denies IPv4-mapped IPv6 public DNS result", SocketOperation::ConnectEx,
         reinterpret_cast<const sockaddr*>(&ipv4MappedDnsResult), sizeof(ipv4MappedDnsResult),
         NetworkDecision::DenyNonLoopback},
        {"broadcast is denied", SocketOperation::SendTo,
         reinterpret_cast<const sockaddr*>(&ipv4Broadcast), sizeof(ipv4Broadcast),
         NetworkDecision::DenyNonLoopback},
        {"IPv4 multicast is denied", SocketOperation::SendTo,
         reinterpret_cast<const sockaddr*>(&ipv4Multicast), sizeof(ipv4Multicast),
         NetworkDecision::DenyNonLoopback},
        {"IPv6 multicast is denied", SocketOperation::SendTo,
         reinterpret_cast<const sockaddr*>(&ipv6Multicast), sizeof(ipv6Multicast),
         NetworkDecision::DenyNonLoopback},
        {"short IPv4 length is denied", SocketOperation::Connect,
         reinterpret_cast<const sockaddr*>(&ipv4Loopback), sizeof(ipv4Loopback) - 1,
         NetworkDecision::DenyNonLoopback},
        {"oversized IPv6 length is denied", SocketOperation::Connect,
         reinterpret_cast<const sockaddr*>(&ipv6Loopback), sizeof(ipv6Loopback) + 1,
         NetworkDecision::DenyNonLoopback},
        {"null address is denied", SocketOperation::Connect, nullptr, 0,
         NetworkDecision::DenyNonLoopback},
    };

    for (const auto& test : cases) {
        if (const auto result = RequireDecision(
                test.name, test.operation, test.address, test.length, test.expected);
            result != 0) {
            return result;
        }
    }

    sockaddr unspecified{};
    unspecified.sa_family = AF_UNSPEC;
    if (const auto result = RequireDecision(
            "AF_UNSPEC is denied", SocketOperation::Connect, &unspecified,
            sizeof(unspecified), NetworkDecision::DenyNonLoopback);
        result != 0) {
        return result;
    }

    sockaddr unknown{};
    unknown.sa_family = static_cast<ADDRESS_FAMILY>(0x7fff);
    if (const auto result = RequireDecision(
            "unknown family is denied", SocketOperation::Connect, &unknown,
            sizeof(unknown), NetworkDecision::DenyNonLoopback);
        result != 0) {
        return result;
    }

    return RequireDecision(
        "unknown operation is denied", static_cast<SocketOperation>(-1),
        reinterpret_cast<const sockaddr*>(&ipv4Loopback), sizeof(ipv4Loopback),
        NetworkDecision::DenyNonLoopback);
}

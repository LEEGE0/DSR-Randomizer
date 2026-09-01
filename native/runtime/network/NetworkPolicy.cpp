#include "network/NetworkPolicy.h"

#include <cstring>

namespace DSRRandomizer::Network {
namespace {

bool IsSupportedOperation(const SocketOperation operation) noexcept {
    switch (operation) {
    case SocketOperation::Connect:
    case SocketOperation::WsaConnect:
    case SocketOperation::SendTo:
    case SocketOperation::ConnectEx:
        return true;
    }
    return false;
}

bool IsIPv4Loopback(const IN_ADDR& address) noexcept {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&address);
    return bytes[0] == 127;
}

bool IsIPv4MappedIPv6(const IN6_ADDR& address) noexcept {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&address);
    for (int index = 0; index < 10; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return bytes[10] == 0xff && bytes[11] == 0xff;
}

bool IsIPv6Loopback(const IN6_ADDR& address) noexcept {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&address);
    for (int index = 0; index < 15; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return bytes[15] == 1;
}

NetworkDecision ParseIPv4(const sockaddr* const address, const int length) noexcept {
    if (length != static_cast<int>(sizeof(sockaddr_in))) {
        return NetworkDecision::DenyNonLoopback;
    }

    sockaddr_in endpoint{};
    std::memcpy(&endpoint, address, sizeof(endpoint));
    return endpoint.sin_family == AF_INET && IsIPv4Loopback(endpoint.sin_addr)
        ? NetworkDecision::AllowLoopback
        : NetworkDecision::DenyNonLoopback;
}

NetworkDecision ParseIPv6(const sockaddr* const address, const int length) noexcept {
    if (length != static_cast<int>(sizeof(sockaddr_in6))) {
        return NetworkDecision::DenyNonLoopback;
    }

    sockaddr_in6 endpoint{};
    std::memcpy(&endpoint, address, sizeof(endpoint));
    if (endpoint.sin6_family != AF_INET6) {
        return NetworkDecision::DenyNonLoopback;
    }

    const auto* const bytes = reinterpret_cast<const unsigned char*>(&endpoint.sin6_addr);
    return IsIPv6Loopback(endpoint.sin6_addr)
            || (IsIPv4MappedIPv6(endpoint.sin6_addr) && bytes[12] == 127)
        ? NetworkDecision::AllowLoopback
        : NetworkDecision::DenyNonLoopback;
}

}  // namespace

NetworkDecision EvaluateSocketOperation(
    const SocketOperation operation,
    const sockaddr* const address,
    const int length) noexcept {
    if (!IsSupportedOperation(operation) || address == nullptr
        || length < static_cast<int>(sizeof(ADDRESS_FAMILY))) {
        return NetworkDecision::DenyNonLoopback;
    }

    ADDRESS_FAMILY family{};
    std::memcpy(&family, address, sizeof(family));
    switch (family) {
    case AF_INET:
        return ParseIPv4(address, length);
    case AF_INET6:
        return ParseIPv6(address, length);
    default:
        return NetworkDecision::DenyNonLoopback;
    }
}

}  // namespace DSRRandomizer::Network

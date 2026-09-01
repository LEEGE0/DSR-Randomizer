#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

namespace DSRRandomizer::Network {

enum class SocketOperation {
    Connect,
    WsaConnect,
    SendTo,
    ConnectEx,
};

enum class NetworkDecision {
    AllowLoopback,
    DenyNonLoopback,
};

[[nodiscard]] NetworkDecision EvaluateSocketOperation(
    SocketOperation operation,
    const sockaddr* address,
    int length) noexcept;

}  // namespace DSRRandomizer::Network

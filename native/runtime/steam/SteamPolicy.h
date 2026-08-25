#pragma once

#include <string_view>

namespace DSRRandomizer::Steam {

enum class SteamMethod {
    CreateLobby,
    RequestLobbyList,
    JoinLobby,
    AdvertiseLobby,
    SendP2PPacket,
    AcceptP2PSession,
    ReceiveP2PPacket,
    FileWrite,
    FileRead,
    FileDelete,
    FileShare,
    CloudSynchronize,
    GetSteamID,
};

enum class MethodDecision {
    Allow,
    Deny,
    UnknownInterfaceFatal,
};

enum class InterfaceDecision {
    Wrap,
    AllowOwnershipIdentity,
    AllowRaw,
    UnknownProtectedFatal,
    Unrecognized,
};

class SteamPolicy final {
public:
    [[nodiscard]] InterfaceDecision EvaluateInterface(
        std::string_view version) const noexcept;
    [[nodiscard]] MethodDecision Evaluate(
        std::string_view version,
        SteamMethod method) const noexcept;
};

}  // namespace DSRRandomizer::Steam

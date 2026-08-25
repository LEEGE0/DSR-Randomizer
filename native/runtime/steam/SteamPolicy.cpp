#include "steam/SteamPolicy.h"

namespace DSRRandomizer::Steam {
namespace {

constexpr std::string_view kMatchmaking = "SteamMatchMaking009";
constexpr std::string_view kNetworking = "SteamNetworking006";
constexpr std::string_view kRemoteStorage =
    "STEAMREMOTESTORAGE_INTERFACE_VERSION016";
constexpr std::string_view kUser = "SteamUser023";

bool HasPrefix(
    const std::string_view value,
    const std::string_view prefix) noexcept {
    return value.size() >= prefix.size()
        && value.substr(0, prefix.size()) == prefix;
}

bool IsProtectedPrefix(const std::string_view version) noexcept {
    return HasPrefix(version, "SteamMatchMaking")
        || HasPrefix(version, "SteamNetworking")
        || HasPrefix(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION");
}

bool IsMatchmakingMethod(const SteamMethod method) noexcept {
    return method == SteamMethod::CreateLobby
        || method == SteamMethod::RequestLobbyList
        || method == SteamMethod::JoinLobby
        || method == SteamMethod::AdvertiseLobby;
}

bool IsNetworkingMethod(const SteamMethod method) noexcept {
    return method == SteamMethod::SendP2PPacket
        || method == SteamMethod::AcceptP2PSession
        || method == SteamMethod::ReceiveP2PPacket;
}

bool IsRemoteStorageMethod(const SteamMethod method) noexcept {
    return method == SteamMethod::FileWrite
        || method == SteamMethod::FileRead
        || method == SteamMethod::FileDelete
        || method == SteamMethod::FileShare
        || method == SteamMethod::CloudSynchronize;
}

}  // namespace

InterfaceDecision SteamPolicy::EvaluateInterface(
    const std::string_view version) const noexcept {
    if (version == kMatchmaking
        || version == kNetworking
        || version == kRemoteStorage) {
        return InterfaceDecision::Wrap;
    }
    if (version == kUser) {
        return InterfaceDecision::AllowOwnershipIdentity;
    }
    return IsProtectedPrefix(version)
        ? InterfaceDecision::UnknownProtectedFatal
        : InterfaceDecision::Unrecognized;
}

MethodDecision SteamPolicy::Evaluate(
    const std::string_view version,
    const SteamMethod method) const noexcept {
    const auto interfaceDecision = EvaluateInterface(version);
    if (interfaceDecision == InterfaceDecision::UnknownProtectedFatal) {
        return MethodDecision::UnknownInterfaceFatal;
    }
    if (version == kMatchmaking && IsMatchmakingMethod(method)) {
        return MethodDecision::Deny;
    }
    if (version == kNetworking && IsNetworkingMethod(method)) {
        return MethodDecision::Deny;
    }
    if (version == kRemoteStorage && IsRemoteStorageMethod(method)) {
        return MethodDecision::Deny;
    }
    if (version == kUser && method == SteamMethod::GetSteamID) {
        return MethodDecision::Allow;
    }
    return interfaceDecision == InterfaceDecision::Wrap
        ? MethodDecision::Deny
        : MethodDecision::UnknownInterfaceFatal;
}

}  // namespace DSRRandomizer::Steam

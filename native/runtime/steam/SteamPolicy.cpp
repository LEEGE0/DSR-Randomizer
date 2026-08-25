#include "steam/SteamPolicy.h"

#include <algorithm>
#include <array>

namespace DSRRandomizer::Steam {
namespace {

constexpr std::string_view kMatchmaking = "SteamMatchMaking009";
constexpr std::string_view kNetworking = "SteamNetworking006";
constexpr std::string_view kPinnedNetworking = "SteamNetworking005";
constexpr std::string_view kRemoteStorage =
    "STEAMREMOTESTORAGE_INTERFACE_VERSION016";
constexpr std::string_view kPinnedRemoteStorage =
    "STEAMREMOTESTORAGE_INTERFACE_VERSION014";
constexpr std::string_view kUser = "SteamUser023";
constexpr std::string_view kPinnedUser = "SteamUser019";

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

constexpr std::array<std::string_view, 17> kPinnedRawInterfaces{
    "SteamClient017",
    "SteamFriends015",
    "SteamUtils009",
    "SteamMatchMakingServers002",
    "STEAMUSERSTATS_INTERFACE_VERSION011",
    "STEAMAPPS_INTERFACE_VERSION008",
    "STEAMSCREENSHOTS_INTERFACE_VERSION003",
    "STEAMHTTP_INTERFACE_VERSION002",
    "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001",
    "SteamController005",
    "STEAMUGC_INTERFACE_VERSION010",
    "STEAMAPPLIST_INTERFACE_VERSION001",
    "STEAMMUSIC_INTERFACE_VERSION001",
    "STEAMMUSICREMOTE_INTERFACE_VERSION001",
    "STEAMHTMLSURFACE_INTERFACE_VERSION_003",
    "STEAMINVENTORY_INTERFACE_V002",
    "STEAMVIDEO_INTERFACE_V002",
};

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
        || version == kPinnedNetworking
        || version == kRemoteStorage
        || version == kPinnedRemoteStorage) {
        return InterfaceDecision::Wrap;
    }
    if (version == kUser || version == kPinnedUser) {
        return InterfaceDecision::AllowOwnershipIdentity;
    }
    if (std::find(
            kPinnedRawInterfaces.begin(),
            kPinnedRawInterfaces.end(),
            version) != kPinnedRawInterfaces.end()) {
        return InterfaceDecision::AllowRaw;
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
    if ((version == kNetworking || version == kPinnedNetworking)
        && IsNetworkingMethod(method)) {
        return MethodDecision::Deny;
    }
    if ((version == kRemoteStorage || version == kPinnedRemoteStorage)
        && IsRemoteStorageMethod(method)) {
        return MethodDecision::Deny;
    }
    if ((version == kUser || version == kPinnedUser)
        && method == SteamMethod::GetSteamID) {
        return MethodDecision::Allow;
    }
    return interfaceDecision == InterfaceDecision::Wrap
        ? MethodDecision::Deny
        : MethodDecision::UnknownInterfaceFatal;
}

}  // namespace DSRRandomizer::Steam

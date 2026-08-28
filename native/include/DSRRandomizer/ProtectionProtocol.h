#pragma once

#include <cstddef>
#include <cstdint>

namespace DSRRandomizer {

inline constexpr std::uint32_t kProtectionMagic = 0x44535252;
inline constexpr std::uint16_t kProtectionProtocolVersion = 2;
inline constexpr std::uint64_t kSimplifiedOfflineRequiredFlags =
    (1ULL << 7) - 1ULL;
inline constexpr std::size_t kProtectionNonceSize = 32;
inline constexpr std::size_t kProtectionPipeNameCharacters = 128;
inline constexpr std::size_t kProtectionSavePathCharacters = 512;
inline constexpr std::size_t kProtectionSocketEndpointCapacity = 2;
inline constexpr std::size_t kProtectionDeniedCounterCount = 6;

enum class SocketTransport : std::uint16_t {
    Tcp = 1,
    Udp = 2,
};

enum class ProtectionFlags : std::uint64_t {
    None = 0,
    Bootstrap = 1ULL << 0,
    SaveKnownFolder = 1ULL << 1,
    SaveFileIo = 1ULL << 2,
    Winsock = 1ULL << 3,
    SteamInterfaces = 1ULL << 4,
    DeferredModuleGate = 1ULL << 5,
    GameServiceOffline = 1ULL << 6,
    Heartbeat = 1ULL << 7,
    HookIntegrity = 1ULL << 8,
};

enum class ProtectionMessageKind : std::uint32_t {
    Handshake = 1,
    Heartbeat = 2,
    Fatal = 3,
};

enum class ProtectionFatalCode : std::uint32_t {
    HookIntegrityFailed = 1,
    HeartbeatStopped = 2,
    ProtectionThreadFailed = 3,
};

#pragma pack(push, 1)
struct ProtectionSocketEndpoint {
    std::uint16_t transport;
    std::uint16_t family;
    std::uint16_t port;
    std::uint16_t reserved;
    std::uint8_t address[16];
};

struct ProtectionInitBlock {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint64_t requiredFlags;
    std::uint32_t diagnosticMode;
    std::uint8_t nonce[kProtectionNonceSize];
    wchar_t pipeName[kProtectionPipeNameCharacters];
    wchar_t virtualDocuments[kProtectionSavePathCharacters];
    wchar_t virtualLogicalSave[kProtectionSavePathCharacters];
    wchar_t realSaveRoot[kProtectionSavePathCharacters];
    wchar_t externalSaveRoot[kProtectionSavePathCharacters];
    wchar_t dedicatedRmm[kProtectionSavePathCharacters];
    std::uint32_t socketEndpointCount;
    ProtectionSocketEndpoint socketEndpoints[kProtectionSocketEndpointCapacity];
};

struct ProtectionHandshakeMessage {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint8_t nonce[kProtectionNonceSize];
    std::uint32_t kind;
    std::uint32_t status;
    std::uint64_t activeFlags;
};

struct ProtectionHeartbeatMessage {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint8_t nonce[kProtectionNonceSize];
    std::uint32_t kind;
    std::uint64_t sequence;
    std::uint64_t monotonicMilliseconds;
    std::uint64_t activeFlags;
    std::uint64_t deniedCounters[kProtectionDeniedCounterCount];
};

struct ProtectionFatalMessage {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint8_t nonce[kProtectionNonceSize];
    std::uint32_t kind;
    std::uint32_t fatalCode;
};
#pragma pack(pop)

static_assert(sizeof(ProtectionHandshakeMessage) == 56);
static_assert(sizeof(ProtectionHeartbeatMessage) == 116);
static_assert(sizeof(ProtectionFatalMessage) == 48);

}  // namespace DSRRandomizer

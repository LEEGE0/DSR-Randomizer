#pragma once

#include <cstddef>
#include <cstdint>

namespace DSRRandomizer {

inline constexpr std::uint32_t kProtectionMagic = 0x44535252;
inline constexpr std::uint16_t kProtectionProtocolVersion = 2;
inline constexpr std::size_t kProtectionNonceSize = 32;
inline constexpr std::size_t kProtectionPipeNameCharacters = 128;
inline constexpr std::size_t kProtectionSavePathCharacters = 512;

enum class ProtectionFlags : std::uint64_t {
    None = 0,
    Bootstrap = 1ULL << 0,
    SaveKnownFolder = 1ULL << 1,
    SaveFileIo = 1ULL << 2,
};

#pragma pack(push, 1)
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
};

struct ProtectionHandshakeMessage {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint8_t nonce[kProtectionNonceSize];
    std::uint32_t status;
    std::uint64_t activeFlags;
};
#pragma pack(pop)

}  // namespace DSRRandomizer

#pragma once

#include <cstdint>

namespace DSRRandomizer {

inline constexpr std::uint32_t kProtectionMagic = 0x44535252;
inline constexpr std::uint16_t kProtectionProtocolVersion = 1;

enum class ProtectionFlags : std::uint64_t {
    None = 0,
    Bootstrap = 1ULL << 0,
};

#pragma pack(push, 1)
struct ProtectionInitBlock {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
};
#pragma pack(pop)

}  // namespace DSRRandomizer

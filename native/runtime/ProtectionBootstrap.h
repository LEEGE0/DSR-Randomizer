#pragma once

#include "DSRRandomizer/ProtectionProtocol.h"

namespace DSRRandomizer {

enum class InitStatus : std::uint32_t {
    Success = 0,
    InvalidArgument = 1,
    UnsupportedProtocol = 2,
};

InitStatus InitializeProtection(ProtectionInitBlock* block) noexcept;
InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept;
ProtectionFlags CurrentProtectionFlags() noexcept;

}  // namespace DSRRandomizer

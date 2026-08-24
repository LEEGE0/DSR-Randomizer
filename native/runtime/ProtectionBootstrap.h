#pragma once

#include <cstddef>
#include <string>

#include "DSRRandomizer/ProtectionProtocol.h"

namespace DSRRandomizer {

enum class InitStatus : std::uint32_t {
    Success = 0,
    InvalidArgument = 1,
    UnsupportedProtocol = 2,
    RequiredProtectionUnavailable = 3,
    SupervisorUnavailable = 4,
    SupervisorReportFailed = 5,
    SaveHookInstallFailed = 6,
    WinsockHookInstallFailed = 7,
};

InitStatus InitializeProtection(ProtectionInitBlock* block) noexcept;
InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept;
ProtectionFlags CurrentProtectionFlags() noexcept;

namespace Testing {

using RequiredPathReader = bool(*)(
    const wchar_t*,
    std::size_t,
    std::wstring&);

InitStatus InitializeWithPathReader(
    ProtectionInitBlock* block,
    RequiredPathReader reader) noexcept;

}  // namespace Testing

}  // namespace DSRRandomizer

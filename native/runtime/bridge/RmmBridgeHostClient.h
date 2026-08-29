#pragma once

#include <cstdint>
#include <string>

#include "bridge/RmmBridgeConfiguration.h"

namespace DSRRandomizer::Bridge {

[[nodiscard]] std::wstring BuildHostCommandLine(
    const BridgeConfiguration& configuration,
    std::uint32_t gamePid,
    const std::wstring& readyEventName);

[[nodiscard]] bool StartRmmBridgeHostAndWaitReady(
    const BridgeConfiguration& configuration,
    std::wstring& message);

}  // namespace DSRRandomizer::Bridge

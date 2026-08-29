#pragma once

#include <string>
#include <string_view>

#include "bridge/RmmBridgeConfiguration.h"
#include "save/SaveHooks.h"

namespace DSRRandomizer::Bridge {

struct BridgeBootstrapResult {
    bool ok{};
    std::uint32_t exitCode{};
    std::wstring message;
};

class BridgeBootstrapPlatform {
public:
    virtual ~BridgeBootstrapPlatform() = default;
    [[nodiscard]] virtual BridgeConfigurationResult ResolveConfiguration() = 0;
    [[nodiscard]] virtual bool StartHostAndWaitReady(
        const BridgeConfiguration& configuration,
        std::wstring& message) = 0;
    [[nodiscard]] virtual bool InstallHooks(
        const Save::SaveHookConfiguration& configuration,
        std::wstring& message) = 0;
    virtual void WriteFailureLog(
        const BridgeConfiguration* configuration,
        std::wstring_view message) = 0;
};

[[nodiscard]] BridgeBootstrapResult BootstrapRmmBridge(
    BridgeBootstrapPlatform& platform) noexcept;

}  // namespace DSRRandomizer::Bridge

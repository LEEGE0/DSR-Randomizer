#pragma once

#include "bridge/RmmBridgeBootstrap.h"
#include "bridge/RmmBridgeConfiguration.h"

namespace DSRRandomizer::Bridge {

class WindowsBridgePlatform final
    : public BridgeConfigurationPlatform,
      public BridgeBootstrapPlatform {
public:
    [[nodiscard]] std::wstring ProcessImagePath() const override;
    [[nodiscard]] std::wstring DocumentsPath() const override;
    [[nodiscard]] bool ReadBoundedUtf8(
        const std::wstring& path,
        std::size_t maximumBytes,
        std::string& content) const override;
    [[nodiscard]] bool CanonicalizeExisting(
        const std::wstring& path,
        std::wstring& canonical) const override;
    [[nodiscard]] bool InspectFile(
        const std::wstring& path,
        FileInspection& inspection) const override;
    [[nodiscard]] bool Sha256File(
        const std::wstring& path,
        std::string& sha256) const override;

    [[nodiscard]] BridgeConfigurationResult ResolveConfiguration() override;
    [[nodiscard]] bool StartHostAndWaitReady(
        const BridgeConfiguration& configuration,
        std::wstring& message) override;
    [[nodiscard]] bool InstallHooks(
        const Save::SaveHookConfiguration& configuration,
        std::wstring& message) override;
    void WriteFailureLog(
        const BridgeConfiguration* configuration,
        std::wstring_view message) override;
};

}  // namespace DSRRandomizer::Bridge

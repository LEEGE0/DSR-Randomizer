#pragma once

#include <optional>

#include "bridge/RmmBridgeBootstrap.h"
#include "bridge/RmmBridgeConfiguration.h"
#include "save/SaveCallsiteRedirect.h"

namespace DSRRandomizer::Bridge {

[[nodiscard]] std::wstring DeriveExternalRootFromBridgeModulePath(
    std::wstring_view modulePath);

class WindowsBridgePlatform final
    : public BridgeConfigurationPlatform,
      public BridgeBootstrapPlatform {
public:
    [[nodiscard]] std::wstring ProcessImagePath() const override;
    [[nodiscard]] std::wstring ExternalRootPath() const override;
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
    [[nodiscard]] bool PrepareCallsiteRedirect(
        const std::wstring& dedicatedRmm,
        std::wstring& message) override;
    [[nodiscard]] bool InstallCallsiteRedirect(
        std::wstring& message) override;
    void WriteFailureLog(
        const BridgeConfiguration* configuration,
        std::wstring_view message) override;

private:
    std::optional<Save::SaveCallsiteRedirectConfiguration> preparedCallsite_;
};

}  // namespace DSRRandomizer::Bridge

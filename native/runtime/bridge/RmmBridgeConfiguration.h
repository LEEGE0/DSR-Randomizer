#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace DSRRandomizer::Bridge {

enum class BridgeConfigurationError {
    None,
    ProcessImageInvalid,
    LayoutInvalid,
    ConfigurationMissing,
    ConfigurationMalformed,
    ConfigurationTooLarge,
    RuntimeMismatch,
    SteamIdInvalid,
    SaveInvalid,
    MetadataInvalid,
    UnsupportedMetadataSchema,
    FileAliasRejected,
};

struct FileInspection {
    bool regularFile{};
    bool exists{};
    bool reparsePoint{};
    std::uint32_t linkCount{};
    std::uint64_t length{};
};

struct BridgeConfiguration {
    std::wstring runtimeRoot;
    std::wstring externalRoot;
    std::wstring runtimeId;
    std::wstring steamId;
    std::wstring virtualDocuments;
    std::wstring virtualLogicalSave;
    std::wstring realSaveRoot;
    std::wstring externalSaveRoot;
    std::wstring dedicatedRmm;
    std::wstring hostExecutable;
    std::string saveIdentity;
    std::string metadataIdentity;
    std::wstring overhaulGameParamSource;
    std::wstring overhaulGameParamTarget;
};

struct BridgeConfigurationResult {
    bool ok{};
    BridgeConfiguration value;
    BridgeConfigurationError error{BridgeConfigurationError::None};
    std::wstring message;
};

class BridgeConfigurationPlatform {
public:
    virtual ~BridgeConfigurationPlatform() = default;
    [[nodiscard]] virtual std::wstring ProcessImagePath() const = 0;
    [[nodiscard]] virtual std::wstring ExternalRootPath() const = 0;
    [[nodiscard]] virtual std::wstring DocumentsPath() const = 0;
    [[nodiscard]] virtual bool ReadBoundedUtf8(
        const std::wstring& path,
        std::size_t maximumBytes,
        std::string& content) const = 0;
    [[nodiscard]] virtual bool CanonicalizeExisting(
        const std::wstring& path,
        std::wstring& canonical) const = 0;
    [[nodiscard]] virtual bool InspectFile(
        const std::wstring& path,
        FileInspection& inspection) const = 0;
    [[nodiscard]] virtual bool Sha256File(
        const std::wstring& path,
        std::string& sha256) const = 0;
};

[[nodiscard]] BridgeConfigurationResult ResolveBridgeConfiguration(
    const BridgeConfigurationPlatform& platform);

}  // namespace DSRRandomizer::Bridge

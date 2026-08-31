#include <iostream>
#include <map>
#include <string>
#include <string_view>

#include "bridge/RmmBridgeConfiguration.h"

namespace {

using DSRRandomizer::Bridge::BridgeConfigurationError;
using DSRRandomizer::Bridge::BridgeConfigurationPlatform;
using DSRRandomizer::Bridge::FileInspection;
using DSRRandomizer::Bridge::ResolveBridgeConfiguration;

constexpr auto kRoot = LR"(D:\DSR MOD)";
constexpr auto kRuntimeId = L"runtime-a39cb5e0";

std::wstring Join(std::wstring_view left, std::wstring_view right) {
    return std::wstring(left) + L"\\" + std::wstring(right);
}

class FakePlatform final : public BridgeConfigurationPlatform {
public:
    FakePlatform() {
        processImage = Join(Join(Join(kRoot, L"runtimes"), kRuntimeId),
                            L"DarkSoulsRemastered.exe");
        documents = LR"(C:\Users\FixtureUser\Documents)";
        const auto pointerPath = Join(kRoot, L"runtime-current.json");
        const auto selectionPath = Join(Join(kRoot, L"config"),
                                        L"selected-save-profile.json");
        const auto metadataPath = Join(Join(Join(kRoot, L"saves"), L"146808034"),
                                       L"save-metadata.json");
        const auto savePath = Join(Join(Join(kRoot, L"saves"), L"146808034"),
                                   L"DRAKS0005.rmm");
        hashes[processImage] =
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
        files[pointerPath] =
            R"({"runtimeId":"runtime-a39cb5e0","relativeRuntimePath":"runtimes/runtime-a39cb5e0","manifestSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
        files[selectionPath] =
            R"({"steamId":"146808034","sourcePath":"C:\\Users\\User\\Documents\\NBGI\\DARK SOULS REMASTERED\\146808034\\DRAKS0005.sl2"})";
        files[metadataPath] =
            R"({"schemaVersion":1,"steamId":"146808034","fixedLength":4326608,"lastKnownSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","activeSeedId":null,"placementSha256":null,"cleanExit":true})";
        inspections[savePath] = FileInspection{true, true, false, 1, 4'326'608};
        hashes[savePath] =
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    }

    std::wstring ProcessImagePath() const override { return processImage; }
    std::wstring ExternalRootPath() const override { return kRoot; }
    std::wstring DocumentsPath() const override { return documents; }

    bool ReadBoundedUtf8(const std::wstring& path,
                         std::size_t maximumBytes,
                         std::string& content) const override {
        const auto found = files.find(path);
        if (found == files.end() || found->second.size() > maximumBytes) {
            return false;
        }
        content = found->second;
        return true;
    }

    bool CanonicalizeExisting(const std::wstring& path,
                              std::wstring& canonical) const override {
        const auto found = canonicalPaths.find(path);
        canonical = found == canonicalPaths.end() ? path : found->second;
        return true;
    }

    bool InspectFile(const std::wstring& path,
                     FileInspection& inspection) const override {
        const auto found = inspections.find(path);
        if (found == inspections.end()) {
            return false;
        }
        inspection = found->second;
        return true;
    }

    bool Sha256File(const std::wstring& path, std::string& sha256) const override {
        const auto found = hashes.find(path);
        if (found == hashes.end()) {
            return false;
        }
        sha256 = found->second;
        return true;
    }

    std::wstring processImage;
    std::wstring documents;
    std::map<std::wstring, std::string, std::less<>> files;
    std::map<std::wstring, std::wstring, std::less<>> canonicalPaths;
    std::map<std::wstring, FileInspection, std::less<>> inspections;
    std::map<std::wstring, std::string, std::less<>> hashes;
};

int Fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main() {
    FakePlatform platform;
    const auto valid = ResolveBridgeConfiguration(platform);
    if (!valid.ok) {
        std::wcerr << L"valid configuration rejected: " << valid.message << L'\n';
        return 1;
    }
    if (valid.value.externalRoot != kRoot
        || valid.value.runtimeId != kRuntimeId
        || valid.value.steamId != L"146808034"
        || valid.value.dedicatedRmm
            != LR"(D:\DSR MOD\saves\146808034\DRAKS0005.rmm)"
        || valid.value.overhaulGameParamSource
            != LR"(D:\DSR MOD\runtimes\runtime-a39cb5e0\overhaul\GameParam.parambnd.dcx)"
        || valid.value.overhaulGameParamTarget
            != LR"(D:\DSR MOD\components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx)") {
        return Fail("resolved configuration does not match the canonical layout");
    }

    FakePlatform hardlinkedRuntime;
    const auto launchedRuntimeImage = hardlinkedRuntime.processImage;
    hardlinkedRuntime.processImage =
        LR"(C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe)";
    hardlinkedRuntime.canonicalPaths[launchedRuntimeImage] =
        hardlinkedRuntime.processImage;
    hardlinkedRuntime.hashes[hardlinkedRuntime.processImage] =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    const auto hardlinked = ResolveBridgeConfiguration(hardlinkedRuntime);
    if (!hardlinked.ok || hardlinked.value.externalRoot != kRoot
        || hardlinked.value.runtimeId != kRuntimeId
        || hardlinked.value.overhaulGameParamSource
            != LR"(C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\overhaul\GameParam.parambnd.dcx)"
        || hardlinked.value.overhaulGameParamTarget
            != LR"(D:\DSR MOD\components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx)") {
        return Fail("hard-linked runtime image did not preserve the live Steam GameParam source");
    }

    FakePlatform differentGameBinary;
    differentGameBinary.processImage =
        LR"(C:\Different Game\DarkSoulsRemastered.exe)";
    differentGameBinary.hashes[differentGameBinary.processImage] =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    if (ResolveBridgeConfiguration(differentGameBinary).error
        != BridgeConfigurationError::RuntimeMismatch) {
        return Fail("different live game binary was not rejected");
    }

    FakePlatform wrongExecutable;
    wrongExecutable.processImage = Join(Join(Join(kRoot, L"runtimes"), kRuntimeId),
                                        L"not-the-game.exe");
    if (ResolveBridgeConfiguration(wrongExecutable).error
        != BridgeConfigurationError::ProcessImageInvalid) {
        return Fail("wrong process image leaf was not rejected");
    }

    FakePlatform duplicateJson;
    duplicateJson.files[Join(kRoot, L"runtime-current.json")] =
        R"({"runtimeId":"runtime-a39cb5e0","runtimeId":"runtime-a39cb5e0","relativeRuntimePath":"runtimes/runtime-a39cb5e0","manifestSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    if (ResolveBridgeConfiguration(duplicateJson).error
        != BridgeConfigurationError::ConfigurationMalformed) {
        return Fail("duplicate JSON property was not rejected");
    }

    FakePlatform wrongLength;
    const auto savePath = Join(Join(Join(kRoot, L"saves"), L"146808034"),
                               L"DRAKS0005.rmm");
    const auto metadataPath = Join(Join(Join(kRoot, L"saves"), L"146808034"),
                                   L"save-metadata.json");
    wrongLength.inspections[savePath].length = 10;
    if (ResolveBridgeConfiguration(wrongLength).error
        != BridgeConfigurationError::SaveInvalid) {
        return Fail("wrong dedicated-save length was not rejected");
    }

    FakePlatform reparseSave;
    reparseSave.inspections[savePath].reparsePoint = true;
    if (ResolveBridgeConfiguration(reparseSave).error
        != BridgeConfigurationError::FileAliasRejected) {
        return Fail("reparse-point save was not rejected");
    }

    FakePlatform mismatchedHash;
    mismatchedHash.hashes[savePath] =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    if (ResolveBridgeConfiguration(mismatchedHash).error
        != BridgeConfigurationError::MetadataInvalid) {
        return Fail("metadata/save hash mismatch was not rejected");
    }

    FakePlatform uncleanSave;
    uncleanSave.files[metadataPath] =
        R"({"schemaVersion":1,"steamId":"146808034","fixedLength":4326608,"lastKnownSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","activeSeedId":null,"placementSha256":null,"cleanExit":false})";
    uncleanSave.hashes[savePath] =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    const auto recoveredUncleanSave = ResolveBridgeConfiguration(uncleanSave);
    if (!recoveredUncleanSave.ok) {
        std::wcerr << L"unclean dedicated save was not accepted for recovery: "
                   << recoveredUncleanSave.message << L'\n';
        return 1;
    }

    FakePlatform malformedCleanExit;
    malformedCleanExit.files[metadataPath] =
        R"({"schemaVersion":1,"steamId":"146808034","fixedLength":4326608,"lastKnownSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","activeSeedId":null,"placementSha256":null,"cleanExit":"false"})";
    if (ResolveBridgeConfiguration(malformedCleanExit).error
        != BridgeConfigurationError::MetadataInvalid) {
        return Fail("non-boolean cleanExit metadata was not rejected");
    }

    FakePlatform unsupportedSchema;
    unsupportedSchema.files[metadataPath] = R"({"schemaVersion":2})";
    if (ResolveBridgeConfiguration(unsupportedSchema).error
        != BridgeConfigurationError::UnsupportedMetadataSchema) {
        return Fail("unsupported metadata schema was not rejected");
    }

    return 0;
}

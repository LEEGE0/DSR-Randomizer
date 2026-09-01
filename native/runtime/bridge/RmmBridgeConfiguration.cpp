#include "bridge/RmmBridgeConfiguration.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <variant>

namespace DSRRandomizer::Bridge {
namespace {

constexpr std::size_t kMaximumConfigurationBytes = 65'536;
constexpr std::uint64_t kDedicatedSaveLength = 4'326'608;

using JsonValue = std::variant<std::nullptr_t, std::string, std::int64_t, bool>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

class FlatJsonParser {
public:
    explicit FlatJsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::optional<JsonObject> Parse() {
        SkipWhitespace();
        if (!Consume('{')) {
            return std::nullopt;
        }
        JsonObject result;
        SkipWhitespace();
        if (Consume('}')) {
            return result;
        }
        while (true) {
            auto key = ParseString();
            if (!key || result.contains(*key)) {
                return std::nullopt;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return std::nullopt;
            }
            SkipWhitespace();
            auto value = ParseValue();
            if (!value) {
                return std::nullopt;
            }
            result.emplace(std::move(*key), std::move(*value));
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                return std::nullopt;
            }
            SkipWhitespace();
        }
        SkipWhitespace();
        return position_ == input_.size() ? std::optional<JsonObject>(std::move(result))
                                          : std::nullopt;
    }

private:
    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const auto value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    std::optional<std::string> ParseString() {
        SkipWhitespace();
        if (!Consume('"')) {
            return std::nullopt;
        }
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') {
                return result;
            }
            if (value < 0x20U || value >= 0x80U) {
                return std::nullopt;
            }
            if (value != '\\') {
                result.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) {
                return std::nullopt;
            }
            const auto escaped = input_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseValue() {
        if (position_ >= input_.size()) {
            return std::nullopt;
        }
        if (input_[position_] == '"') {
            auto value = ParseString();
            return value ? std::optional<JsonValue>(std::move(*value)) : std::nullopt;
        }
        if (input_.substr(position_, 4) == "null") {
            position_ += 4;
            return JsonValue(nullptr);
        }
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            return JsonValue(true);
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            return JsonValue(false);
        }
        const auto start = position_;
        if (input_[position_] == '-') {
            ++position_;
        }
        while (position_ < input_.size()
               && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (position_ == start || (position_ == start + 1 && input_[start] == '-')) {
            return std::nullopt;
        }
        std::int64_t number{};
        const auto [end, error] = std::from_chars(
            input_.data() + start, input_.data() + position_, number);
        if (error != std::errc{} || end != input_.data() + position_) {
            return std::nullopt;
        }
        return JsonValue(number);
    }

    std::string_view input_;
    std::size_t position_{};
};

BridgeConfigurationResult Failure(BridgeConfigurationError error,
                                  std::wstring message) {
    return BridgeConfigurationResult{false, {}, error, std::move(message)};
}

std::wstring NormalizeSeparators(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') {
        value.pop_back();
    }
    return value;
}

std::wstring Parent(std::wstring_view path) {
    const auto normalized = NormalizeSeparators(std::wstring(path));
    const auto separator = normalized.find_last_of(L'\\');
    return separator == std::wstring::npos ? std::wstring{} : normalized.substr(0, separator);
}

std::wstring Leaf(std::wstring_view path) {
    const auto normalized = NormalizeSeparators(std::wstring(path));
    const auto separator = normalized.find_last_of(L'\\');
    return separator == std::wstring::npos ? normalized : normalized.substr(separator + 1);
}

std::wstring Join(std::wstring_view left, std::wstring_view right) {
    auto result = NormalizeSeparators(std::wstring(left));
    result.push_back(L'\\');
    result.append(right);
    return NormalizeSeparators(std::move(result));
}

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
        return std::towlower(a) == std::towlower(b);
    });
}

std::optional<std::wstring> AsciiWide(const JsonObject& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !std::holds_alternative<std::string>(found->second)) {
        return std::nullopt;
    }
    const auto& source = std::get<std::string>(found->second);
    if (std::any_of(source.begin(), source.end(), [](unsigned char value) {
            return value >= 0x80U;
        })) {
        return std::nullopt;
    }
    return std::wstring(source.begin(), source.end());
}

std::optional<std::string> StringValue(const JsonObject& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !std::holds_alternative<std::string>(found->second)) {
        return std::nullopt;
    }
    return std::get<std::string>(found->second);
}

std::optional<std::int64_t> IntegerValue(const JsonObject& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !std::holds_alternative<std::int64_t>(found->second)) {
        return std::nullopt;
    }
    return std::get<std::int64_t>(found->second);
}

std::optional<bool> BooleanValue(const JsonObject& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !std::holds_alternative<bool>(found->second)) {
        return std::nullopt;
    }
    return std::get<bool>(found->second);
}

bool HasExactly(const JsonObject& object, const std::set<std::string, std::less<>>& keys) {
    if (object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&](const auto& key) {
        return object.contains(key);
    });
}

bool IsDecimalSteamId(std::wstring_view steamId) {
    return !steamId.empty() && steamId.size() <= 20
        && std::all_of(steamId.begin(), steamId.end(), [](wchar_t value) {
            return value >= L'0' && value <= L'9';
        });
}

bool IsHexSha256(std::string_view value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
}

std::optional<JsonObject> ReadObject(const BridgeConfigurationPlatform& platform,
                                     const std::wstring& path) {
    std::string content;
    if (!platform.ReadBoundedUtf8(path, kMaximumConfigurationBytes, content)) {
        return std::nullopt;
    }
    return FlatJsonParser(content).Parse();
}

}  // namespace

BridgeConfigurationResult ResolveBridgeConfiguration(
    const BridgeConfigurationPlatform& platform) {
    auto processImage = NormalizeSeparators(platform.ProcessImagePath());
    if (!EqualsInsensitive(Leaf(processImage), L"DarkSoulsRemastered.exe")) {
        return Failure(BridgeConfigurationError::ProcessImageInvalid,
                       L"The bridge is not loaded in DarkSoulsRemastered.exe.");
    }
    std::wstring resolvedProcessImage;
    if (!platform.CanonicalizeExisting(processImage, resolvedProcessImage)
        || !EqualsInsensitive(
            Leaf(NormalizeSeparators(resolvedProcessImage)),
            L"DarkSoulsRemastered.exe")) {
        return Failure(BridgeConfigurationError::ProcessImageInvalid,
                       L"The live process image could not be resolved.");
    }

    const auto externalRoot = NormalizeSeparators(platform.ExternalRootPath());
    if (externalRoot.empty()) {
        return Failure(BridgeConfigurationError::LayoutInvalid,
                       L"The bridge DLL is not below the external components directory.");
    }

    const auto pointerPath = Join(externalRoot, L"runtime-current.json");
    const auto pointer = ReadObject(platform, pointerPath);
    const std::set<std::string, std::less<>> pointerKeys{
        "manifestSha256", "relativeRuntimePath", "runtimeId"};
    if (!pointer || !HasExactly(*pointer, pointerKeys)) {
        return Failure(BridgeConfigurationError::ConfigurationMalformed,
                       L"runtime-current.json is missing or malformed.");
    }
    const auto configuredRuntimeId = AsciiWide(*pointer, "runtimeId");
    const auto relativeRuntimePath = AsciiWide(*pointer, "relativeRuntimePath");
    const auto manifestHash = StringValue(*pointer, "manifestSha256");
    if (!configuredRuntimeId || !relativeRuntimePath || !manifestHash
        || !IsHexSha256(*manifestHash)
        || relativeRuntimePath->find(L"..") != std::wstring::npos
        || (!relativeRuntimePath->empty()
            && ((*relativeRuntimePath)[0] == L'\\' || relativeRuntimePath->find(L':') != std::wstring::npos))) {
        return Failure(BridgeConfigurationError::ConfigurationMalformed,
                       L"The active runtime pointer contains invalid values.");
    }
    const auto runtimeRoot = Join(
        externalRoot, NormalizeSeparators(*relativeRuntimePath));
    const auto runtimeId = Leaf(runtimeRoot);
    const auto configuredProcessImage = Join(runtimeRoot, L"DarkSoulsRemastered.exe");
    std::wstring configuredRuntime;
    std::wstring resolvedConfiguredProcessImage;
    std::string liveProcessHash;
    std::string configuredProcessHash;
    if (runtimeId.empty()
        || !EqualsInsensitive(Leaf(Parent(runtimeRoot)), L"runtimes")
        || !platform.CanonicalizeExisting(runtimeRoot, configuredRuntime)
        || !platform.CanonicalizeExisting(
            configuredProcessImage, resolvedConfiguredProcessImage)
        || !platform.Sha256File(processImage, liveProcessHash)
        || !platform.Sha256File(configuredProcessImage, configuredProcessHash)
        || liveProcessHash != configuredProcessHash
        || !EqualsInsensitive(*configuredRuntimeId, runtimeId)) {
        return Failure(BridgeConfigurationError::RuntimeMismatch,
                       L"The live process does not match the active runtime pointer.");
    }

    const auto selectionPath = Join(Join(externalRoot, L"config"),
                                    L"selected-save-profile.json");
    const auto selection = ReadObject(platform, selectionPath);
    const std::set<std::string, std::less<>> selectionKeys{"sourcePath", "steamId"};
    if (!selection || !HasExactly(*selection, selectionKeys)) {
        return Failure(BridgeConfigurationError::ConfigurationMalformed,
                       L"The selected save profile is missing or malformed.");
    }
    const auto steamId = AsciiWide(*selection, "steamId");
    const auto sourcePath = AsciiWide(*selection, "sourcePath");
    if (!steamId || !sourcePath || !IsDecimalSteamId(*steamId)) {
        return Failure(BridgeConfigurationError::SteamIdInvalid,
                       L"The selected Steam ID is invalid.");
    }
    if (!EqualsInsensitive(Leaf(*sourcePath), L"DRAKS0005.sl2")
        || Leaf(Parent(*sourcePath)) != *steamId) {
        return Failure(BridgeConfigurationError::ConfigurationMalformed,
                       L"The selected source path does not match the Steam ID.");
    }

    const auto externalSaveRoot = Join(Join(externalRoot, L"saves"), *steamId);
    const auto dedicatedRmm = Join(externalSaveRoot, L"DRAKS0005.rmm");
    const auto metadataPath = Join(externalSaveRoot, L"save-metadata.json");
    const auto metadata = ReadObject(platform, metadataPath);
    if (!metadata) {
        return Failure(BridgeConfigurationError::MetadataInvalid,
                       L"Dedicated-save metadata is missing or malformed.");
    }
    const auto schemaVersion = IntegerValue(*metadata, "schemaVersion");
    if (!schemaVersion || *schemaVersion != 1) {
        return Failure(BridgeConfigurationError::UnsupportedMetadataSchema,
                       L"Dedicated-save metadata schema is unsupported.");
    }
    const std::set<std::string, std::less<>> metadataKeys{
        "activeSeedId", "cleanExit", "fixedLength", "lastKnownSha256",
        "placementSha256", "schemaVersion", "steamId"};
    const auto metadataSteamId = AsciiWide(*metadata, "steamId");
    const auto fixedLength = IntegerValue(*metadata, "fixedLength");
    const auto expectedHash = StringValue(*metadata, "lastKnownSha256");
    const auto cleanExit = BooleanValue(*metadata, "cleanExit");
    if (!HasExactly(*metadata, metadataKeys) || !metadataSteamId || !fixedLength
        || !expectedHash || !cleanExit || *metadataSteamId != *steamId
        || *fixedLength != static_cast<std::int64_t>(kDedicatedSaveLength)
        || !IsHexSha256(*expectedHash)) {
        return Failure(BridgeConfigurationError::MetadataInvalid,
                       L"Dedicated-save metadata does not match the selected profile.");
    }

    FileInspection inspection;
    if (!platform.InspectFile(dedicatedRmm, inspection) || !inspection.exists
        || !inspection.regularFile || inspection.length != kDedicatedSaveLength) {
        return Failure(BridgeConfigurationError::SaveInvalid,
                       L"The dedicated save is missing or has the wrong length.");
    }
    if (inspection.reparsePoint || inspection.linkCount != 1) {
        return Failure(BridgeConfigurationError::FileAliasRejected,
                       L"The dedicated save is linked or is a reparse point.");
    }
    std::string actualHash;
    if (!platform.Sha256File(dedicatedRmm, actualHash)
        || (*cleanExit && actualHash != *expectedHash)) {
        return Failure(BridgeConfigurationError::MetadataInvalid,
                       L"The dedicated save hash does not match its metadata.");
    }

    const auto virtualDocuments = Join(externalRoot, L"profile");
    const auto virtualLogicalSave = Join(
        Join(Join(Join(virtualDocuments, L"NBGI"), L"DARK SOULS REMASTERED"),
             *steamId),
        L"DRAKS0005.sl2");
    const auto realSaveRoot = Join(
        Join(platform.DocumentsPath(), L"NBGI"), L"DARK SOULS REMASTERED");
    const auto hostExecutable = Join(
        Join(Join(externalRoot, L"components"), L"rmm-bridge"),
        L"DSRRandomizer.RmmBridgeHost.exe");
    const auto overhaulGameParamSource = Join(
        Parent(processImage), L"overhaul\\GameParam.parambnd.dcx");
    const auto overhaulGameParamTarget = Join(
        Join(Join(Join(externalRoot, L"components"), L"rmm-bridge"), L"content"),
        L"overhaul\\GameParam.parambnd.dcx");

    BridgeConfiguration result{
        runtimeRoot,
        externalRoot,
        runtimeId,
        *steamId,
        virtualDocuments,
        virtualLogicalSave,
        realSaveRoot,
        externalSaveRoot,
        dedicatedRmm,
        hostExecutable,
        actualHash,
        *expectedHash,
        overhaulGameParamSource,
        overhaulGameParamTarget,
    };
    return BridgeConfigurationResult{true, std::move(result),
                                     BridgeConfigurationError::None, L""};
}

}  // namespace DSRRandomizer::Bridge

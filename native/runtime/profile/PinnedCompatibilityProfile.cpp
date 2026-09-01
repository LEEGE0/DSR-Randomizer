#include "profile/PinnedCompatibilityProfile.h"

#include <Windows.h>
#include <Psapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game/CompatibilityProfile.generated.h"

namespace DSRRandomizer::Profile {
namespace {

struct UniqueHandle final {
    HANDLE value = INVALID_HANDLE_VALUE;

    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : value(handle) {}
    ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr) {
                CloseHandle(value);
            }
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

struct IdentityLease final {
    UniqueHandle executable;
    UniqueHandle steam;
};

struct ExpectedIdentity {
    std::uint64_t length;
    std::array<std::uint8_t, 32> sha256;
    std::uint16_t machine;
    std::uint32_t timestamp;
    std::uint32_t imageSize;
};

struct FileHeaders {
    std::uint16_t machine = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t imageSize = 0;
};

UniqueHandle OpenPinnedReadOnly(const std::wstring& path) noexcept {
    return UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
}

bool ReadCanonicalPath(const HANDLE file, std::wstring& path) {
    const auto required = GetFinalPathNameByHandleW(file, nullptr, 0, 0);
    if (required == 0 || required > 32768) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
    const auto length = GetFinalPathNameByHandleW(
        file, buffer.data(), static_cast<DWORD>(buffer.size()), 0);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return path.starts_with(L"\\\\?\\");
}

bool ReadFileIdentity(const HANDLE file, FILE_ID_INFO& identity) noexcept {
    return GetFileInformationByHandleEx(
        file, FileIdInfo, &identity, sizeof(identity)) != FALSE;
}

bool SameFileIdentity(const HANDLE left, const HANDLE right) noexcept {
    FILE_ID_INFO leftIdentity{};
    FILE_ID_INFO rightIdentity{};
    return ReadFileIdentity(left, leftIdentity)
        && ReadFileIdentity(right, rightIdentity)
        && leftIdentity.VolumeSerialNumber == rightIdentity.VolumeSerialNumber
        && std::equal(
            std::begin(leftIdentity.FileId.Identifier),
            std::end(leftIdentity.FileId.Identifier),
            std::begin(rightIdentity.FileId.Identifier));
}

UniqueHandle OpenMappedBacking(const HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetMappedFileNameW(
        GetCurrentProcess(),
        reinterpret_cast<void*>(module),
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    std::wstring devicePath(buffer.data(), length);
    if (!devicePath.starts_with(L"\\Device\\")) {
        return {};
    }
    const std::wstring globalPath = L"\\\\?\\GLOBALROOT" + devicePath;
    return UniqueHandle(CreateFileW(
        globalPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
}

bool HashFile(
    const HANDLE file,
    std::array<std::uint8_t, 32>& result) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0;
    DWORD bytesRead = 0;
    bool success = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (success) {
        success = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytesRead,
            0) == 0
            && bytesRead == sizeof(objectLength)
            && objectLength != 0;
    }
    std::vector<std::uint8_t> hashObject;
    if (success) {
        hashObject.resize(objectLength);
        success = BCryptCreateHash(
            algorithm,
            &hash,
            hashObject.data(),
            static_cast<ULONG>(hashObject.size()),
            nullptr,
            0,
            0) == 0;
    }
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (success) {
        DWORD count = 0;
        if (!ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &count,
                nullptr)) {
            success = false;
            break;
        }
        if (count == 0) {
            break;
        }
        success = BCryptHashData(hash, buffer.data(), count, 0) == 0;
    }
    if (success) {
        success = BCryptFinishHash(
            hash,
            result.data(),
            static_cast<ULONG>(result.size()),
            0) == 0;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return success;
}

bool ReadHeaders(const HANDLE file, FileHeaders& headers) noexcept {
    LARGE_INTEGER position{};
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
        || !ReadFile(file, &dos, sizeof(dos), &read, nullptr)
        || read != sizeof(dos)
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0) {
        return false;
    }
    position.QuadPart = dos.e_lfanew;
    IMAGE_NT_HEADERS64 nt{};
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
        || !ReadFile(file, &nt, sizeof(nt), &read, nullptr)
        || read != sizeof(nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    headers = {
        nt.FileHeader.Machine,
        nt.FileHeader.TimeDateStamp,
        nt.OptionalHeader.SizeOfImage,
    };
    return true;
}

bool VerifyFile(
    const HANDLE file,
    const ExpectedIdentity& expected) {
    LARGE_INTEGER length{};
    FileHeaders headers{};
    std::array<std::uint8_t, 32> sha256{};
    return GetFileSizeEx(file, &length)
        && length.QuadPart >= 0
        && static_cast<std::uint64_t>(length.QuadPart) == expected.length
        && ReadHeaders(file, headers)
        && headers.machine == expected.machine
        && headers.timestamp == expected.timestamp
        && headers.imageSize == expected.imageSize
        && HashFile(file, sha256)
        && sha256 == expected.sha256;
}

bool VerifyMappedHeaders(
    const HMODULE module,
    const ExpectedIdentity& expected) noexcept {
    __try {
        const auto* const base = reinterpret_cast<const std::byte*>(module);
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            return false;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE
            && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
            && nt->FileHeader.Machine == expected.machine
            && nt->FileHeader.TimeDateStamp == expected.timestamp
            && nt->OptionalHeader.SizeOfImage == expected.imageSize;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool VerifyMappedModule(
    const HMODULE module,
    const HANDLE expectedFile,
    const ExpectedIdentity& expected) {
    auto backing = OpenMappedBacking(module);
    return backing.value != INVALID_HANDLE_VALUE
        && SameFileIdentity(backing.value, expectedFile)
        && VerifyMappedHeaders(module, expected);
}

std::wstring ModulePath(const HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring AdjacentPath(
    const std::wstring& executableCanonicalPath,
    const wchar_t* const baseName) {
    const auto slash = executableCanonicalPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return executableCanonicalPath.substr(0, slash + 1) + baseName;
}

void RetainDenyOnlyFatal(const char*) noexcept {
    // Task 5 supplies authenticated fatal-event transport. The gate itself
    // enters and retains deny-only mode before calling this reporter.
}

void PopulateSaveCallsiteProfile(
    const HMODULE executableModule,
    const std::shared_ptr<void>& identityLease,
    Save::SaveCallsiteRedirectConfiguration& configuration) {
    configuration = {};
    configuration.identityLease = identityLease;
    configuration.targets = {{
        Save::SaveCallsiteRedirectTarget{
            reinterpret_cast<std::byte*>(executableModule) + 0xD051DF,
            {
                0xff, 0x15, 0x57, 0x22, 0x31, 0x01,
                0x48, 0x89, 0x46, 0x60, 0x48, 0x83, 0xf8, 0xff,
            },
            0,
        },
        Save::SaveCallsiteRedirectTarget{
            reinterpret_cast<std::byte*>(executableModule) + 0xD045BF,
            {
                0x33, 0xd2, 0xff, 0x15, 0x75, 0x2e, 0x31, 0x01,
                0x48, 0x8b, 0xf8, 0x40, 0xb6, 0x01,
            },
            2,
        },
    }};
}

}  // namespace

#if defined(DSR_RANDOMIZER_RMM_BRIDGE_INTEGRATION_PROFILE)
namespace {

using IntegrationCallsiteProvider = bool (*) (
    Save::SaveCallsiteRedirectTarget*, std::size_t);

PinnedCompatibilityProfileStatus BuildIntegrationSaveCallsiteProfile(
    Save::SaveCallsiteRedirectConfiguration& configuration) noexcept {
    try {
        configuration = {};
        const auto executableModule = GetModuleHandleW(nullptr);
        const auto provider = executableModule == nullptr
            ? nullptr
            : reinterpret_cast<IntegrationCallsiteProvider>(GetProcAddress(
                executableModule, "DsrGetRmmBridgeIntegrationSaveCallsites"));
        if (provider == nullptr
            || !provider(configuration.targets.data(), configuration.targets.size())) {
            configuration = {};
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        for (const auto& target : configuration.targets) {
            if (target.address == nullptr
                || target.callOffset > Save::kSaveCallsiteFingerprintSize - 6
                || target.expected[target.callOffset] != 0xff
                || target.expected[target.callOffset + 1] != 0x15
                || std::memcmp(
                    target.address,
                    target.expected.data(),
                    target.expected.size()) != 0) {
                configuration = {};
                return PinnedCompatibilityProfileStatus::ProfileMismatch;
            }
        }
        return PinnedCompatibilityProfileStatus::Success;
    }
    catch (...) {
        configuration = {};
        return PinnedCompatibilityProfileStatus::InvalidConfiguration;
    }
}

}  // namespace

PinnedCompatibilityProfileStatus BuildPinnedSaveCallsiteProfile(
    Save::SaveCallsiteRedirectConfiguration& configuration) noexcept {
    return BuildIntegrationSaveCallsiteProfile(configuration);
}
#else
PinnedCompatibilityProfileStatus BuildPinnedSaveCallsiteProfile(
    Save::SaveCallsiteRedirectConfiguration& configuration) noexcept {
    try {
        using namespace Game::Generated;
        configuration = {};
        const auto executableModule = GetModuleHandleW(kExecutableModule);
        if (executableModule == nullptr
            || executableModule != GetModuleHandleW(nullptr)) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        const auto executablePath = ModulePath(executableModule);
        if (executablePath.empty()) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        auto lease = std::make_shared<IdentityLease>();
        lease->executable = OpenPinnedReadOnly(executablePath);
        std::wstring executableCanonicalPath;
        const ExpectedIdentity executableIdentity{
            kExecutableLength,
            kExecutableSha256,
            kMachine,
            kTimestamp,
            kSizeOfImage,
        };
        if (lease->executable.value == INVALID_HANDLE_VALUE
            || !ReadCanonicalPath(
                lease->executable.value, executableCanonicalPath)
            || !VerifyFile(lease->executable.value, executableIdentity)
            || !VerifyMappedModule(
                executableModule,
                lease->executable.value,
                executableIdentity)) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        PopulateSaveCallsiteProfile(executableModule, lease, configuration);
        return PinnedCompatibilityProfileStatus::Success;
    }
    catch (...) {
        configuration = {};
        return PinnedCompatibilityProfileStatus::InvalidConfiguration;
    }
}
#endif

PinnedCompatibilityProfileStatus BuildPinnedCompatibilityProfile(
    PinnedCompatibilityProfile& profile) noexcept {
    try {
        using namespace Game::Generated;
        profile = {};
        const auto executableModule = GetModuleHandleW(kExecutableModule);
        if (executableModule == nullptr
            || executableModule != GetModuleHandleW(nullptr)) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        const auto executablePath = ModulePath(executableModule);
        if (executablePath.empty()) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }

        auto lease = std::make_shared<IdentityLease>();
        lease->executable = OpenPinnedReadOnly(executablePath);
        std::wstring executableCanonicalPath;
        const ExpectedIdentity executableIdentity{
            kExecutableLength,
            kExecutableSha256,
            kMachine,
            kTimestamp,
            kSizeOfImage,
        };
        if (lease->executable.value == INVALID_HANDLE_VALUE
            || !ReadCanonicalPath(
                lease->executable.value, executableCanonicalPath)
            || !VerifyFile(lease->executable.value, executableIdentity)
            || !VerifyMappedModule(
                executableModule,
                lease->executable.value,
                executableIdentity)) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }

        const auto steamPath = AdjacentPath(
            executableCanonicalPath, kSteamModule);
        lease->steam = OpenPinnedReadOnly(steamPath);
        std::wstring steamCanonicalPath;
        const ExpectedIdentity steamIdentity{
            kSteamLength,
            kSteamSha256,
            kSteamMachine,
            kSteamTimestamp,
            kSteamSizeOfImage,
        };
        if (steamPath.empty()
            || lease->steam.value == INVALID_HANDLE_VALUE
            || !ReadCanonicalPath(lease->steam.value, steamCanonicalPath)
            || !VerifyFile(lease->steam.value, steamIdentity)) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }

        const auto steamModule = GetModuleHandleW(kSteamModule);
        if ((!kSteamAllowDeferred && steamModule == nullptr)
            || (steamModule != nullptr
                && !VerifyMappedModule(
                    steamModule, lease->steam.value, steamIdentity))) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }
        if (steamModule != nullptr
            && std::any_of(
                kSteamProtectedFactoryExports.begin(),
                kSteamProtectedFactoryExports.end(),
                [steamModule](const std::string_view name) {
                    return GetProcAddress(steamModule, name.data()) == nullptr;
                })) {
            return PinnedCompatibilityProfileStatus::ProfileMismatch;
        }

        profile.identityLease = lease;
        PopulateSaveCallsiteProfile(
            executableModule, lease, profile.saveRedirect);
        profile.gameService.identityLease = lease;
        profile.gameService.images.push_back({
            kExecutableModule,
            reinterpret_cast<const std::byte*>(executableModule),
            kSizeOfImage,
        });
        profile.gameService.targets.reserve(kTargets.size());
        for (const auto& target : kTargets) {
            profile.gameService.targets.push_back({
                target.moduleName,
                target.rva,
                target.fingerprintSha256,
                target.patchLength,
                target.action,
            });
        }

        Modules::DeferredModuleExpectation steam{};
        steam.expectedPath = steamCanonicalPath.substr(4);
        steam.expectedSha256 = kSteamSha256;
        steam.allowDeferred = kSteamAllowDeferred;
        for (const auto version : kSteamDeclaredInterfaces) {
            steam.declaredInterfaces.emplace_back(version);
        }
        for (const auto name : kSteamProtectedFactoryExports) {
            steam.protectedFactoryExports.emplace_back(name);
        }
        profile.steam.modules.push_back(std::move(steam));
        profile.steam.fatalReporter = &RetainDenyOnlyFatal;
        profile.steam.identityLease = lease;
        return PinnedCompatibilityProfileStatus::Success;
    }
    catch (...) {
        profile = {};
        return PinnedCompatibilityProfileStatus::InvalidConfiguration;
    }
}

}  // namespace DSRRandomizer::Profile

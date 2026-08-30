#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "hooks/MinHookCoordinator.h"
#include "save/SaveHooks.h"

namespace {

namespace fs = std::filesystem;
using DSRRandomizer::Save::HookPlatform;
using DSRRandomizer::Save::SaveHookCleanupStatus;
using DSRRandomizer::Save::SaveHookConfiguration;
using DSRRandomizer::Save::SaveHookInstallStatus;

struct FixtureHookFailures {
    std::size_t missingTarget = std::numeric_limits<std::size_t>::max();
    std::size_t failedCreate = std::numeric_limits<std::size_t>::max();
    std::size_t failedQueue = std::numeric_limits<std::size_t>::max();
    std::size_t partialEnableCount = 0;
    std::size_t failedRemove = std::numeric_limits<std::size_t>::max();
    bool failApply = false;
    bool failDisable = false;
    bool failUninitialize = false;
};

class FixtureHookPlatform final : public HookPlatform {
public:
    explicit FixtureHookPlatform(FixtureHookFailures failures = {})
        : failures_(failures) {}

    void BeginMutation() noexcept override {
        mutationLease_ = std::make_unique<
            DSRRandomizer::Hooks::MinHookMutationLease>();
    }

    void EndMutation() noexcept override {
        mutationLease_.reset();
        if (mutationReleasedEvent_ != nullptr) {
            SetEvent(mutationReleasedEvent_);
        }
    }

    bool Initialize() noexcept override {
        initialized_ = true;
        return true;
    }

    void* ResolveTarget(const wchar_t*, const char*) noexcept override {
        const auto index = resolveCount_++;
        if (index == failures_.missingTarget) {
            return nullptr;
        }
        return reinterpret_cast<void*>(0x10000ULL + (index * 0x100ULL));
    }

    bool CreateHook(void* target, void*, void** original) noexcept override {
        if (createCount_++ == failures_.failedCreate) {
            return false;
        }
        created_.insert(target);
        *original = target;
        return true;
    }

    bool QueueEnable(void* target) noexcept override {
        if (queueCount_++ == failures_.failedQueue) {
            return false;
        }
        queued_.push_back(target);
        return true;
    }

    bool ApplyQueued() noexcept override {
        applyWasCalled_ = true;
        if (failures_.failApply) {
            const auto enabledCount = (std::min)(
                failures_.partialEnableCount,
                queued_.size());
            enabled_.insert(queued_.begin(), queued_.begin() + enabledCount);
            return false;
        }
        enabled_.insert(queued_.begin(), queued_.end());
        return true;
    }

    bool DisableAll() noexcept override {
        disableWasCalled_ = true;
        if (failures_.failDisable) {
            return false;
        }
        enabled_.clear();
        return true;
    }

    bool RemoveHook(void* target) noexcept override {
        const auto index = static_cast<std::size_t>(
            (reinterpret_cast<std::uintptr_t>(target) - 0x10000ULL) / 0x100ULL);
        if (index == failures_.failedRemove) {
            return false;
        }
        enabled_.erase(target);
        created_.erase(target);
        return true;
    }

    bool Uninitialize() noexcept override {
        if (failures_.failUninitialize) {
            return false;
        }
        initialized_ = false;
        queued_.clear();
        return true;
    }

    [[nodiscard]] bool WasRolledBack() const noexcept {
        return !initialized_ && created_.empty() && enabled_.empty();
    }
    [[nodiscard]] bool ApplyWasCalled() const noexcept { return applyWasCalled_; }
    [[nodiscard]] bool DisableWasCalled() const noexcept { return disableWasCalled_; }
    [[nodiscard]] std::size_t CreatedCount() const noexcept { return created_.size(); }
    [[nodiscard]] std::size_t EnabledCount() const noexcept { return enabled_.size(); }

    void AllowDisable() noexcept { failures_.failDisable = false; }
    void AllowRemove() noexcept {
        failures_.failedRemove = std::numeric_limits<std::size_t>::max();
    }
    void AllowUninitialize() noexcept { failures_.failUninitialize = false; }
    void SignalMutationRelease(const HANDLE event) noexcept {
        mutationReleasedEvent_ = event;
    }

private:
    FixtureHookFailures failures_;
    std::size_t resolveCount_ = 0;
    std::size_t createCount_ = 0;
    std::size_t queueCount_ = 0;
    bool initialized_ = false;
    bool applyWasCalled_ = false;
    bool disableWasCalled_ = false;
    std::set<void*> created_;
    std::set<void*> enabled_;
    std::vector<void*> queued_;
    std::unique_ptr<DSRRandomizer::Hooks::MinHookMutationLease> mutationLease_;
    HANDLE mutationReleasedEvent_ = nullptr;
};

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

std::wstring Quote(const std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

bool CreateDirectories(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    return !error;
}

bool WriteFixtureFile(
    const fs::path& path,
    const std::string_view contents) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL succeeded = WriteFile(
        file,
        contents.data(),
        static_cast<DWORD>(contents.size()),
        &written,
        nullptr);
    CloseHandle(file);
    return succeeded && written == contents.size();
}

bool ReadFixtureFile(
    const fs::path& path,
    std::string& contents) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, 128> buffer{};
    DWORD read = 0;
    const BOOL succeeded = ReadFile(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &read,
        nullptr);
    CloseHandle(file);
    if (!succeeded) {
        return false;
    }
    contents.assign(buffer.data(), read);
    return true;
}

std::string NarrowPath(const std::wstring_view path) {
    const auto required = WideCharToMultiByte(
        CP_ACP,
        0,
        path.data(),
        static_cast<int>(path.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(required), '\0');
    return WideCharToMultiByte(
               CP_ACP,
               0,
               path.data(),
               static_cast<int>(path.size()),
               narrow.data(),
               required,
               nullptr,
               nullptr) == required
        ? narrow
        : std::string{};
}

class CurrentDirectoryLease final {
public:
    explicit CurrentDirectoryLease(const fs::path& replacement) {
        std::array<wchar_t, 32768> current{};
        const auto length = GetCurrentDirectoryW(
            static_cast<DWORD>(current.size()),
            current.data());
        if (length != 0 && length < current.size()) {
            original_.assign(current.data(), length);
        }
        active_ = !original_.empty()
            && SetCurrentDirectoryW(replacement.c_str());
    }

    ~CurrentDirectoryLease() {
        Restore();
    }

    CurrentDirectoryLease(const CurrentDirectoryLease&) = delete;
    CurrentDirectoryLease& operator=(const CurrentDirectoryLease&) = delete;

    [[nodiscard]] bool Active() const noexcept { return active_; }

    void Restore() noexcept {
        if (active_) {
            SetCurrentDirectoryW(original_.c_str());
            active_ = false;
        }
    }

private:
    std::wstring original_;
    bool active_ = false;
};

struct RootSwapAttempt {
    std::wstring approvedRoot;
    std::wstring savedApprovedRoot;
    std::wstring outsideRoot;
    bool attempted = false;
    bool succeeded = false;
};

void AttemptRootSwap(void* state) noexcept {
    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(nullptr, nullptr);
    auto& attempt = *static_cast<RootSwapAttempt*>(state);
    attempt.attempted = true;
    if (!MoveFileW(
            attempt.approvedRoot.c_str(),
            attempt.savedApprovedRoot.c_str())) {
        return;
    }
    if (MoveFileW(attempt.outsideRoot.c_str(), attempt.approvedRoot.c_str())) {
        attempt.succeeded = true;
        return;
    }
    MoveFileW(attempt.savedApprovedRoot.c_str(), attempt.approvedRoot.c_str());
}

fs::path CreateTemporaryRoot() {
    std::array<wchar_t, 32768> temporary{};
    const auto length = GetTempPathW(
        static_cast<DWORD>(temporary.size()),
        temporary.data());
    if (length == 0 || length >= temporary.size()) {
        return {};
    }
    std::array<wchar_t, 32768> longTemporary{};
    const auto longLength = GetLongPathNameW(
        temporary.data(),
        longTemporary.data(),
        static_cast<DWORD>(longTemporary.size()));
    if (longLength == 0 || longLength >= longTemporary.size()) {
        return {};
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = fs::path(longTemporary.data())
            / (L"DSRRandomizer-SaveHook-"
               + std::to_wstring(GetCurrentProcessId())
               + L"-" + std::to_wstring(stamp)
               + L"-" + std::to_wstring(attempt));
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            return {};
        }
    }
    return {};
}

class TemporaryRoot final {
public:
    explicit TemporaryRoot(fs::path path) : path_(std::move(path)) {}
    ~TemporaryRoot() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TemporaryRoot(const TemporaryRoot&) = delete;
    TemporaryRoot& operator=(const TemporaryRoot&) = delete;

    [[nodiscard]] const fs::path& Path() const noexcept { return path_; }

private:
    fs::path path_;
};

bool ReadHandshake(HANDLE pipe) {
    DSRRandomizer::ProtectionHandshakeMessage message{};
    DWORD read = 0;
    return ReadFile(
            pipe,
            &message,
            static_cast<DWORD>(sizeof(message)),
            &read,
            nullptr)
        && read == sizeof(message)
        && message.magic == DSRRandomizer::kProtectionMagic
        && message.version == DSRRandomizer::kProtectionProtocolVersion
        && message.size == sizeof(message)
        && message.kind == static_cast<std::uint32_t>(
            DSRRandomizer::ProtectionMessageKind::Handshake)
        && message.status == 0
        && message.activeFlags
            == (static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Bootstrap)
                | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::SaveKnownFolder)
                | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::SaveFileIo)
                | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Heartbeat)
                | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::HookIntegrity));
}

SaveHookConfiguration HookConfigurationFor(const fs::path& root) {
    const auto virtualDocuments = root / L"virtual-documents";
    const auto profile = virtualDocuments
        / L"NBGI" / L"DARK SOULS REMASTERED" / L"12345678901234567";
    return SaveHookConfiguration{
        virtualDocuments.native(),
        (profile / L"DRAKS0005.sl2").native(),
        (root / L"real-normal").native(),
        (root / L"external").native(),
        (root / L"external" / L"DRAKS0005.rmm").native(),
        true,
        false,
    };
}

SaveHookConfiguration GameParamHookConfigurationFor(
    const fs::path& root,
    fs::path& gameRoot,
    fs::path& source,
    fs::path& target) {
    gameRoot = root / L"game-root";
    source = gameRoot / L"overhaul" / L"GameParam.parambnd.dcx";
    target = root / L"external-root" / L"components" / L"rmm-bridge"
        / L"content" / L"overhaul" / L"GameParam.parambnd.dcx";
    auto configuration = HookConfigurationFor(root);
    configuration.overhaulGameParamSource = source.native();
    configuration.overhaulGameParamTarget = target.native();
    return configuration;
}

bool PrepareGameParamFixture(
    const SaveHookConfiguration& configuration,
    const fs::path& source,
    const fs::path& target) {
    return CreateDirectories(fs::path(configuration.virtualLogicalSave).parent_path())
        && CreateDirectories(configuration.realSaveRoot)
        && CreateDirectories(configuration.externalSaveRoot)
        && CreateDirectories(source.parent_path())
        && CreateDirectories(target.parent_path())
        && WriteFixtureFile(configuration.dedicatedRmm, "dedicated")
        && WriteFixtureFile(source, "steam-source")
        && WriteFixtureFile(target, "generated-target-content")
        && SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

int VerifyGameParamConfiguration(const fs::path& root) {
    const auto testRoot = root / L"game-param-configuration";
    fs::path gameRoot;
    fs::path source;
    fs::path target;
    const auto valid = GameParamHookConfigurationFor(
        testRoot,
        gameRoot,
        source,
        target);
    if (!PrepareGameParamFixture(valid, source, target)) {
        return Fail("GameParam configuration fixture setup failed");
    }

    FixtureHookPlatform validPlatform;
    if (DSRRandomizer::Save::InstallSaveHooks(valid, validPlatform)
            != SaveHookInstallStatus::Success
        || validPlatform.CreatedCount() != 16
        || DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !validPlatform.WasRolledBack()) {
        return Fail("valid GameParam configuration did not install one 16-hook group");
    }

    auto isRejected = [](const SaveHookConfiguration& configuration) {
        FixtureHookPlatform platform;
        return DSRRandomizer::Save::InstallSaveHooks(configuration, platform)
            == SaveHookInstallStatus::InvalidConfiguration;
    };

    auto invalid = valid;
    invalid.overhaulGameParamTarget.clear();
    if (!isRejected(invalid)) {
        return Fail("one-sided GameParam redirect configuration was accepted");
    }
    invalid = valid;
    invalid.overhaulGameParamSource.clear();
    if (!isRejected(invalid)) {
        return Fail("target-only GameParam redirect configuration was accepted");
    }
    invalid = valid;
    std::replace(
        invalid.overhaulGameParamSource.begin(),
        invalid.overhaulGameParamSource.end(),
        L'\\',
        L'/');
    if (!isRejected(invalid)) {
        return Fail("non-canonical GameParam source was accepted");
    }
    invalid = valid;
    invalid.overhaulGameParamSource =
        (gameRoot / L"param" / L"GameParam.parambnd.dcx").native();
    if (!isRejected(invalid)) {
        return Fail("GameParam source with the wrong structural suffix was accepted");
    }
    invalid = valid;
    invalid.overhaulGameParamTarget =
        (testRoot / L"content" / L"overhaul" / L"GameParam.parambnd.dcx").native();
    if (!isRejected(invalid)) {
        return Fail("GameParam target with the wrong structural suffix was accepted");
    }
    invalid = valid;
    invalid.overhaulGameParamTarget =
        (target.parent_path() / L"missing-GameParam.parambnd.dcx").native();
    if (!isRejected(invalid)) {
        return Fail("missing GameParam target was accepted");
    }

    auto unprotected = SaveHookConfiguration{
        valid.virtualDocuments,
        {},
        {},
        {},
        {},
        false,
        false,
        valid.overhaulGameParamSource,
        valid.overhaulGameParamTarget,
    };
    if (!isRejected(unprotected)) {
        return Fail("GameParam redirect was accepted without file-I/O protection");
    }

    if (!DeleteFileW(target.c_str())
        || !CreateDirectoryW(target.c_str(), nullptr)
        || !isRejected(valid)
        || !RemoveDirectoryW(target.c_str())
        || !WriteFixtureFile(target, "generated-target-content")) {
        return Fail("directory GameParam target was not rejected");
    }

    const auto sourceAlias = source.parent_path() / L"source-alias.dcx";
    if (!CreateHardLinkW(sourceAlias.c_str(), source.c_str(), nullptr)
        || !isRejected(valid)
        || !DeleteFileW(sourceAlias.c_str())) {
        return Fail("hard-linked GameParam source was not rejected");
    }
    const auto targetAlias = target.parent_path() / L"target-alias.dcx";
    if (!CreateHardLinkW(targetAlias.c_str(), target.c_str(), nullptr)
        || !isRejected(valid)
        || !DeleteFileW(targetAlias.c_str())) {
        return Fail("hard-linked GameParam target was not rejected");
    }
    std::error_code cleanupError;
    fs::remove_all(testRoot, cleanupError);
    if (cleanupError) {
        return Fail("GameParam configuration fixture cleanup failed");
    }
    return 0;
}

bool HandleMatchesIdentity(
    const HANDLE handle,
    const BY_HANDLE_FILE_INFORMATION& expected) {
    BY_HANDLE_FILE_INFORMATION observed{};
    return handle != INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(handle, &observed)
        && observed.dwVolumeSerialNumber == expected.dwVolumeSerialNumber
        && observed.nFileIndexHigh == expected.nFileIndexHigh
        && observed.nFileIndexLow == expected.nFileIndexLow;
}

bool CaptureFileIdentity(
    const fs::path& path,
    BY_HANDLE_FILE_INFORMATION& identity) {
    const HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool captured = GetFileInformationByHandle(handle, &identity) != FALSE;
    CloseHandle(handle);
    return captured;
}

bool OpenMatchesTarget(
    const std::wstring_view request,
    const bool ansi,
    const DWORD desiredAccess,
    const DWORD creation,
    const DWORD flags,
    const BY_HANDLE_FILE_INFORMATION& targetIdentity,
    DWORD& error) {
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (ansi) {
        const auto narrow = NarrowPath(request);
        if (narrow.empty()) {
            return false;
        }
        handle = CreateFileA(
            narrow.c_str(), desiredAccess, FILE_SHARE_READ, nullptr,
            creation, flags, nullptr);
    }
    else {
        const std::wstring wide(request);
        handle = CreateFileW(
            wide.c_str(), desiredAccess, FILE_SHARE_READ, nullptr,
            creation, flags, nullptr);
    }
    error = GetLastError();
    const bool matched = HandleMatchesIdentity(handle, targetIdentity);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    return matched;
}

bool RequestWasNotRedirected(
    const std::wstring_view request,
    const bool ansi,
    const BY_HANDLE_FILE_INFORMATION& targetIdentity) {
    DWORD error = ERROR_SUCCESS;
    return !OpenMatchesTarget(
        request,
        ansi,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        error);
}

bool FileTimesEqual(const FILETIME& left, const FILETIME& right) {
    return left.dwLowDateTime == right.dwLowDateTime
        && left.dwHighDateTime == right.dwHighDateTime;
}

bool AttributeDataMatches(
    const WIN32_FILE_ATTRIBUTE_DATA& observed,
    const WIN32_FILE_ATTRIBUTE_DATA& expected) {
    return observed.dwFileAttributes == expected.dwFileAttributes
        && FileTimesEqual(observed.ftCreationTime, expected.ftCreationTime)
        && FileTimesEqual(observed.ftLastWriteTime, expected.ftLastWriteTime)
        && observed.nFileSizeHigh == expected.nFileSizeHigh
        && observed.nFileSizeLow == expected.nFileSizeLow;
}

bool AllGameParamApisMatchTarget(
    const std::wstring_view request,
    const BY_HANDLE_FILE_INFORMATION& targetIdentity,
    const WIN32_FILE_ATTRIBUTE_DATA& targetMetadata) {
    DWORD wideOpenError = ERROR_SUCCESS;
    DWORD ansiOpenError = ERROR_SUCCESS;
    const bool wideOpen = OpenMatchesTarget(
        request,
        false,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        wideOpenError);
    const bool ansiOpen = OpenMatchesTarget(
        request,
        true,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        ansiOpenError);
    const std::wstring wideRequest(request);
    const auto ansiRequest = NarrowPath(request);
    WIN32_FILE_ATTRIBUTE_DATA wideMetadata{};
    WIN32_FILE_ATTRIBUTE_DATA ansiMetadata{};
    const auto wideAttributes = GetFileAttributesW(wideRequest.c_str());
    const auto ansiAttributes = GetFileAttributesA(ansiRequest.c_str());
    const bool wideExtended = GetFileAttributesExW(
        wideRequest.c_str(),
        GetFileExInfoStandard,
        &wideMetadata) != FALSE;
    const bool ansiExtended = GetFileAttributesExA(
        ansiRequest.c_str(),
        GetFileExInfoStandard,
        &ansiMetadata) != FALSE;
    return wideOpen
        && ansiOpen
        && wideAttributes == targetMetadata.dwFileAttributes
        && ansiAttributes == targetMetadata.dwFileAttributes
        && wideExtended
        && ansiExtended
        && AttributeDataMatches(wideMetadata, targetMetadata)
        && AttributeDataMatches(ansiMetadata, targetMetadata);
}

bool AllGameParamApisAreAccessDenied(
    const std::wstring_view request,
    const BY_HANDLE_FILE_INFORMATION& targetIdentity) {
    DWORD wideOpenError = ERROR_SUCCESS;
    DWORD ansiOpenError = ERROR_SUCCESS;
    const bool wideOpenMatched = OpenMatchesTarget(
        request,
        false,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        wideOpenError);
    const bool ansiOpenMatched = OpenMatchesTarget(
        request,
        true,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        ansiOpenError);

    const std::wstring wideRequest(request);
    const auto ansiRequest = NarrowPath(request);
    SetLastError(ERROR_SUCCESS);
    const auto wideAttributes = GetFileAttributesW(wideRequest.c_str());
    const auto wideAttributesError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto ansiAttributes = GetFileAttributesA(ansiRequest.c_str());
    const auto ansiAttributesError = GetLastError();
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    SetLastError(ERROR_SUCCESS);
    const bool wideExtended = GetFileAttributesExW(
        wideRequest.c_str(),
        GetFileExInfoStandard,
        &metadata) != FALSE;
    const auto wideExtendedError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const bool ansiExtended = GetFileAttributesExA(
        ansiRequest.c_str(),
        GetFileExInfoStandard,
        &metadata) != FALSE;
    const auto ansiExtendedError = GetLastError();
    return !wideOpenMatched
        && wideOpenError == ERROR_ACCESS_DENIED
        && !ansiOpenMatched
        && ansiOpenError == ERROR_ACCESS_DENIED
        && wideAttributes == INVALID_FILE_ATTRIBUTES
        && wideAttributesError == ERROR_ACCESS_DENIED
        && ansiAttributes == INVALID_FILE_ATTRIBUTES
        && ansiAttributesError == ERROR_ACCESS_DENIED
        && !wideExtended
        && wideExtendedError == ERROR_ACCESS_DENIED
        && !ansiExtended
        && ansiExtendedError == ERROR_ACCESS_DENIED;
}

int VerifyGameParamRedirect(const fs::path& root) {
    const auto testRoot = root / L"game-param-redirect";
    fs::path gameRoot;
    fs::path source;
    fs::path target;
    const auto configuration = GameParamHookConfigurationFor(
        testRoot,
        gameRoot,
        source,
        target);
    const auto alternateSource = testRoot / L"alternate-root" / L"overhaul"
        / L"GameParam.parambnd.dcx";
    const auto attacker = target.parent_path() / L"attacker.dcx";
    const auto savedTarget = target.parent_path() / L"saved-target.dcx";
    const auto sourceAttacker = source.parent_path() / L"source-attacker.dcx";
    const auto savedSource = source.parent_path() / L"saved-source.dcx";
    const auto sourceHardLink = source.parent_path() / L"source-hard-link.dcx";
    if (!PrepareGameParamFixture(configuration, source, target)
        || !CreateDirectories(alternateSource.parent_path())
        || !WriteFixtureFile(alternateSource, "alternate-source")
        || !WriteFixtureFile(attacker, "attacker-content")
        || !WriteFixtureFile(sourceAttacker, "source-attacker-content")) {
        return Fail("GameParam redirect fixture setup failed");
    }

    BY_HANDLE_FILE_INFORMATION targetIdentity{};
    WIN32_FILE_ATTRIBUTE_DATA targetMetadata{};
    if (!CaptureFileIdentity(target, targetIdentity)
        || !GetFileAttributesExW(
            target.c_str(),
            GetFileExInfoStandard,
            &targetMetadata)) {
        return Fail("GameParam target identity or metadata could not be captured");
    }
    CurrentDirectoryLease currentDirectory(gameRoot);
    if (!currentDirectory.Active()) {
        return Fail("GameParam fixture working directory could not be selected");
    }
    if (DSRRandomizer::Save::InstallSaveHooks(configuration)
        != SaveHookInstallStatus::Success) {
        return Fail("GameParam redirect hooks did not install");
    }

    bool passed = true;
    const auto countBefore = DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const std::array<std::wstring, 6> accepted{
        LR"(overhaul\GameParam.parambnd.dcx)",
        LR"(.\overhaul\GameParam.parambnd.dcx)",
        LR"(./overhaul/GameParam.parambnd.dcx)",
        LR"(OvErHaUl/GaMePaRaM.PaRaMbNd.DcX)",
        source.native(),
        std::wstring(source.native()).replace(
            std::wstring(source.native()).find(L"\\overhaul\\"),
            std::wstring_view(L"\\overhaul\\").size(),
            L"/OvErHaUl/"),
    };
    for (std::size_t index = 0; index < accepted.size(); ++index) {
        const auto countBeforePath =
            DSRRandomizer::Save::CurrentGameParamRedirectCount();
        const bool matched = AllGameParamApisMatchTarget(
            accepted[index],
            targetIdentity,
            targetMetadata);
        const auto countAfterPath =
            DSRRandomizer::Save::CurrentGameParamRedirectCount();
        if (!matched || countAfterPath != countBeforePath + 6) {
            std::wcerr << L"accepted path failed at " << index
                       << L" countDelta=" << (countAfterPath - countBeforePath)
                       << L" path="
                       << accepted[index] << L'\n';
        }
        passed = matched
            && countAfterPath == countBeforePath + 6
            && passed;
    }

    constexpr DWORD allowedAccess = GENERIC_READ | GENERIC_EXECUTE
        | READ_CONTROL | SYNCHRONIZE | FILE_READ_DATA | FILE_READ_EA
        | FILE_READ_ATTRIBUTES | FILE_EXECUTE;
    const std::array<DWORD, 10> acceptedAccess{
        0,
        GENERIC_READ,
        GENERIC_EXECUTE,
        READ_CONTROL,
        SYNCHRONIZE,
        FILE_READ_DATA,
        FILE_READ_EA,
        FILE_READ_ATTRIBUTES,
        FILE_EXECUTE,
        allowedAccess,
    };
    for (const auto access : acceptedAccess) {
        DWORD error = ERROR_SUCCESS;
        const bool matched = OpenMatchesTarget(
                LR"(overhaul\GameParam.parambnd.dcx)",
                false,
                access,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                targetIdentity,
                error);
        if (!matched) {
            std::cerr << "accepted access failed: access=" << access
                      << " error=" << error << '\n';
        }
        passed = matched && passed;
    }

    const std::array<DWORD, 11> rejectedAccess{
        GENERIC_WRITE,
        GENERIC_ALL,
        FILE_WRITE_DATA,
        FILE_APPEND_DATA,
        FILE_WRITE_EA,
        FILE_WRITE_ATTRIBUTES,
        DELETE,
        WRITE_DAC,
        WRITE_OWNER,
        MAXIMUM_ALLOWED,
        0x00000200,
    };
    for (std::size_t index = 0; index < rejectedAccess.size(); ++index) {
        DWORD error = ERROR_SUCCESS;
        const bool matched = OpenMatchesTarget(
            LR"(overhaul\GameParam.parambnd.dcx)",
            index % 2 != 0,
            rejectedAccess[index],
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            targetIdentity,
            error);
        if (matched || error != ERROR_ACCESS_DENIED) {
            std::cerr << "rejected access failed at " << index
                      << " access=" << rejectedAccess[index]
                      << " matched=" << matched << " error=" << error << '\n';
        }
        passed = !matched && error == ERROR_ACCESS_DENIED && passed;
    }
    for (const auto creation : {
             CREATE_NEW, CREATE_ALWAYS, OPEN_ALWAYS, TRUNCATE_EXISTING}) {
        DWORD error = ERROR_SUCCESS;
        const bool matched = OpenMatchesTarget(
            LR"(overhaul\GameParam.parambnd.dcx)",
            false,
            0,
            creation,
            FILE_ATTRIBUTE_NORMAL,
            targetIdentity,
            error);
        if (matched || error != ERROR_ACCESS_DENIED) {
            std::cerr << "rejected creation failed: creation=" << creation
                      << " matched=" << matched << " error=" << error << '\n';
        }
        passed = !matched && error == ERROR_ACCESS_DENIED && passed;
    }
    DWORD deleteOnCloseError = ERROR_SUCCESS;
    const bool deleteOnCloseMatched = OpenMatchesTarget(
        LR"(overhaul\GameParam.parambnd.dcx)",
        true,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_FLAG_DELETE_ON_CLOSE,
        targetIdentity,
        deleteOnCloseError);
    passed = !deleteOnCloseMatched
        && deleteOnCloseError == ERROR_ACCESS_DENIED
        && passed;

    const auto sourceText = source.native();
    const auto deviceSource = L"\\\\?\\" + sourceText;
    const auto uncSource = L"\\\\localhost\\C$" + sourceText.substr(2);
    const std::array<std::wstring, 14> rejectedPaths{
        LR"(prefix\overhaul\GameParam.parambnd.dcx)",
        LR"(overhaul\nested\GameParam.parambnd.dcx)",
        LR"(overhaul\GameParam.parambnd.dcx.bak)",
        LR"(overhaul\GameParam.parambnd.dcx:stream)",
        LR"(overhaul\.\GameParam.parambnd.dcx)",
        LR"(overhaul\nested\..\GameParam.parambnd.dcx)",
        LR"(overhaul\\GameParam.parambnd.dcx)",
        LR"(OVERHA~1\GAMEPA~1.DCX)",
        LR"(overhaul.\GameParam.parambnd.dcx)",
        LR"(overhaul\GameParam.parambnd.dcx.)",
        LR"(overhaul\GameParam.parambnd.dcx )",
        alternateSource.native(),
        deviceSource,
        uncSource,
    };
    const auto countBeforeRejected =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    for (const auto& rejected : rejectedPaths) {
        const bool wideRejected =
            RequestWasNotRedirected(rejected, false, targetIdentity);
        const bool ansiRejected =
            RequestWasNotRedirected(rejected, true, targetIdentity);
        if (!wideRejected || !ansiRejected) {
            std::wcerr << L"negative path redirected: " << rejected
                       << L" wide=" << wideRejected
                       << L" ansi=" << ansiRejected << L'\n';
        }
        passed = wideRejected && ansiRejected && passed;
        WIN32_FILE_ATTRIBUTE_DATA rejectedWideAttributes{};
        WIN32_FILE_ATTRIBUTE_DATA rejectedAnsiAttributes{};
        const auto rejectedAnsi = NarrowPath(rejected);
        GetFileAttributesW(rejected.c_str());
        GetFileAttributesA(rejectedAnsi.c_str());
        GetFileAttributesExW(
            rejected.c_str(),
            GetFileExInfoStandard,
            &rejectedWideAttributes);
        GetFileAttributesExA(
            rejectedAnsi.c_str(),
            GetFileExInfoStandard,
            &rejectedAnsiAttributes);
    }
    passed = DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeRejected
        && passed;

    WIN32_FILE_ATTRIBUTE_DATA wideAttributes{};
    WIN32_FILE_ATTRIBUTE_DATA ansiAttributes{};
    const auto relativeAnsi = NarrowPath(LR"(.\overhaul\GameParam.parambnd.dcx)");
    SetLastError(ERROR_SUCCESS);
    const auto attributesW =
        GetFileAttributesW(LR"(overhaul\GameParam.parambnd.dcx)");
    const auto attributesWError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto attributesA = GetFileAttributesA(relativeAnsi.c_str());
    const auto attributesAError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const bool attributesExW = GetFileAttributesExW(
            LR"(OvErHaUl/GameParam.parambnd.dcx)",
            GetFileExInfoStandard,
            &wideAttributes) != FALSE;
    const auto attributesExWError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const bool attributesExA = GetFileAttributesExA(
            relativeAnsi.c_str(),
            GetFileExInfoStandard,
            &ansiAttributes) != FALSE;
    const auto attributesExAError = GetLastError();
    const bool attributesPassed = attributesW == targetMetadata.dwFileAttributes
        && attributesA == targetMetadata.dwFileAttributes
        && attributesExW
        && attributesExA
        && wideAttributes.nFileSizeHigh == 0
        && wideAttributes.nFileSizeLow
            == std::string_view("generated-target-content").size()
        && ansiAttributes.nFileSizeHigh == 0
        && ansiAttributes.nFileSizeLow
            == std::string_view("generated-target-content").size();
    if (!attributesPassed) {
        std::cerr << "attribute redirect failed: W=" << attributesW
                  << "/" << attributesWError
                  << " A=" << attributesA
                  << "/" << attributesAError
                  << " ExW=" << attributesExW << "/" << attributesExWError
                  << " ExA=" << attributesExA << "/" << attributesExAError
                  << " WSize=" << wideAttributes.nFileSizeLow
                  << " ASize=" << ansiAttributes.nFileSizeLow << '\n';
    }
    passed = attributesPassed && passed;

    auto verifyAttributePin = [&](const unsigned int api) {
        RootSwapAttempt attempt{
            target.native(),
            savedTarget.native(),
            attacker.native(),
        };
        DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(
            &AttemptRootSwap,
            &attempt);
        bool succeeded = false;
        WIN32_FILE_ATTRIBUTE_DATA pinAttributes{};
        switch (api) {
        case 0:
            succeeded = GetFileAttributesW(
                LR"(overhaul\GameParam.parambnd.dcx)")
                == targetMetadata.dwFileAttributes;
            break;
        case 1:
            succeeded = GetFileAttributesA(relativeAnsi.c_str())
                == targetMetadata.dwFileAttributes;
            break;
        case 2:
            succeeded = GetFileAttributesExW(
                LR"(overhaul\GameParam.parambnd.dcx)",
                GetFileExInfoStandard,
                &pinAttributes) != FALSE;
            break;
        default:
            succeeded = GetFileAttributesExA(
                relativeAnsi.c_str(),
                GetFileExInfoStandard,
                &pinAttributes) != FALSE;
            break;
        }
        DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(
            nullptr,
            nullptr);
        return attempt.attempted && !attempt.succeeded && succeeded;
    };
    for (unsigned int api = 0; api < 4; ++api) {
        const bool attributePinned = verifyAttributePin(api);
        if (!attributePinned) {
            std::cerr << "attribute pin failed for API " << api << '\n';
        }
        passed = attributePinned && passed;
    }

    RootSwapAttempt pinAttempt{
        target.native(),
        savedTarget.native(),
        attacker.native(),
    };
    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(
        &AttemptRootSwap,
        &pinAttempt);
    DWORD pinError = ERROR_SUCCESS;
    const bool pinnedOpen = OpenMatchesTarget(
        LR"(overhaul\GameParam.parambnd.dcx)",
        false,
        GENERIC_READ,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        targetIdentity,
        pinError);
    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(nullptr, nullptr);
    passed = pinAttempt.attempted
        && !pinAttempt.succeeded
        && pinnedOpen
        && passed;
    if (!pinAttempt.attempted || pinAttempt.succeeded || !pinnedOpen) {
        std::cerr << "pin failed: attempted=" << pinAttempt.attempted
                  << " succeeded=" << pinAttempt.succeeded
                  << " opened=" << pinnedOpen
                  << " error=" << pinError << '\n';
    }

    const auto countAfterSuccessful =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    passed = countAfterSuccessful == countBefore + accepted.size() * 6
            + acceptedAccess.size() + 4 + 4 + 1
        && passed;

    if (!MoveFileW(target.c_str(), savedTarget.c_str())
        || !MoveFileW(attacker.c_str(), target.c_str())) {
        std::cerr << "identity swap setup failed: " << GetLastError() << '\n';
        passed = false;
    }
    const auto countBeforeIdentityFailure =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool targetSwapDenied = AllGameParamApisAreAccessDenied(
        LR"(overhaul\GameParam.parambnd.dcx)",
        targetIdentity);
    passed = targetSwapDenied
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeIdentityFailure
        && passed;
    if (!targetSwapDenied) {
        std::cerr << "target identity replacement did not deny all APIs\n";
    }

    if (!MoveFileW(target.c_str(), attacker.c_str())
        || !MoveFileW(savedTarget.c_str(), target.c_str())) {
        std::cerr << "target identity restore failed: " << GetLastError() << '\n';
        passed = false;
    }
    const auto countBeforeRestoredTarget =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool targetRestored = AllGameParamApisMatchTarget(
        source.native(),
        targetIdentity,
        targetMetadata);
    passed = targetRestored
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeRestoredTarget + 6
        && passed;

    if (!MoveFileW(source.c_str(), savedSource.c_str())) {
        std::cerr << "source removal setup failed: " << GetLastError() << '\n';
        passed = false;
    }
    const auto countBeforeMissingSource =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool missingSourceDenied = AllGameParamApisAreAccessDenied(
        LR"(.\overhaul\GameParam.parambnd.dcx)",
        targetIdentity);
    passed = missingSourceDenied
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeMissingSource
        && passed;

    if (!MoveFileW(sourceAttacker.c_str(), source.c_str())) {
        std::cerr << "source replacement setup failed: " << GetLastError() << '\n';
        passed = false;
    }
    const auto countBeforeReplacedSource =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool replacedSourceDenied = AllGameParamApisAreAccessDenied(
        source.native(),
        targetIdentity);
    passed = replacedSourceDenied
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeReplacedSource
        && passed;

    if (!MoveFileW(source.c_str(), sourceAttacker.c_str())
        || !MoveFileW(savedSource.c_str(), source.c_str())) {
        std::cerr << "source identity restore failed: " << GetLastError() << '\n';
        passed = false;
    }
    if (!CreateHardLinkW(sourceHardLink.c_str(), source.c_str(), nullptr)) {
        std::cerr << "source hard-link setup failed: " << GetLastError() << '\n';
        passed = false;
    }
    const auto countBeforeHardLinkedSource =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool hardLinkedSourceDenied = AllGameParamApisAreAccessDenied(
        LR"(OvErHaUl/GameParam.parambnd.dcx)",
        targetIdentity);
    passed = hardLinkedSourceDenied
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeHardLinkedSource
        && passed;
    if (!DeleteFileW(sourceHardLink.c_str())) {
        std::cerr << "source hard-link cleanup failed: " << GetLastError() << '\n';
        passed = false;
    }

    const auto countBeforeRestoredSource =
        DSRRandomizer::Save::CurrentGameParamRedirectCount();
    const bool sourceRestored = AllGameParamApisMatchTarget(
        LR"(overhaul\GameParam.parambnd.dcx)",
        targetIdentity,
        targetMetadata);
    passed = sourceRestored
        && DSRRandomizer::Save::CurrentGameParamRedirectCount()
            == countBeforeRestoredSource + 6
        && passed;
    if (!missingSourceDenied || !replacedSourceDenied
        || !hardLinkedSourceDenied || !targetRestored || !sourceRestored) {
        std::cerr << "source identity fail-closed matrix failed: missing="
                  << missingSourceDenied
                  << " replaced=" << replacedSourceDenied
                  << " hardLinked=" << hardLinkedSourceDenied
                  << " targetRestored=" << targetRestored
                  << " sourceRestored=" << sourceRestored << '\n';
    }

    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(nullptr, nullptr);
    const auto uninstallStatus = DSRRandomizer::Save::UninstallSaveHooks();
    std::string sourceContents;
    std::string originalTargetContents;
    passed = uninstallStatus == SaveHookCleanupStatus::Success
        && ReadFixtureFile(source, sourceContents)
        && sourceContents == "steam-source"
        && ReadFixtureFile(target, originalTargetContents)
        && originalTargetContents == "generated-target-content"
        && passed;
    currentDirectory.Restore();
    std::error_code cleanupError;
    fs::remove_all(testRoot, cleanupError);
    passed = !cleanupError && passed;
    return passed
        ? 0
        : Fail("GameParam redirect path/access/identity matrix failed");
}

int VerifyHookInstallRollback(const fs::path& root) {
    const auto configuration = HookConfigurationFor(root);

    FixtureHookPlatform missingTarget(FixtureHookFailures{.missingTarget = 3});
    const auto missingTargetStatus =
        DSRRandomizer::Save::InstallSaveHooks(configuration, missingTarget);
    if (missingTargetStatus != SaveHookInstallStatus::InstallFailed
        || !missingTarget.WasRolledBack()
        || missingTarget.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        std::cerr << "missing target state: status="
                  << static_cast<unsigned int>(missingTargetStatus)
                  << " rolledBack=" << missingTarget.WasRolledBack()
                  << " apply=" << missingTarget.ApplyWasCalled()
                  << " created=" << missingTarget.CreatedCount()
                  << " enabled=" << missingTarget.EnabledCount()
                  << " installed="
                  << DSRRandomizer::Save::SaveHooksAreInstalled() << '\n';
        return Fail("missing hook target did not roll back the save group");
    }

    FixtureHookPlatform failedCreate(FixtureHookFailures{.failedCreate = 3});
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedCreate)
            != SaveHookInstallStatus::InstallFailed
        || !failedCreate.WasRolledBack()
        || failedCreate.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("partial hook creation failure did not roll back the save group");
    }

    FixtureHookPlatform failedQueue(FixtureHookFailures{.failedQueue = 4});
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedQueue)
            != SaveHookInstallStatus::InstallFailed
        || !failedQueue.WasRolledBack()
        || failedQueue.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("partial hook queue failure did not roll back the save group");
    }

    FixtureHookPlatform failedApply(FixtureHookFailures{
        .partialEnableCount = 3,
        .failApply = true,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedApply)
            != SaveHookInstallStatus::InstallFailed
        || !failedApply.WasRolledBack()
        || !failedApply.ApplyWasCalled()
        || !failedApply.DisableWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("atomic hook apply failure did not disable and roll back the save group");
    }

    FixtureHookPlatform failedDisable(FixtureHookFailures{
        .partialEnableCount = 3,
        .failApply = true,
        .failDisable = true,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedDisable)
            != SaveHookInstallStatus::InstallFailed
        || DSRRandomizer::Save::SaveHooksAreInstalled()
        || failedDisable.EnabledCount() != 3
        || failedDisable.CreatedCount() != 16) {
        return Fail("partial enable plus disable failure was not retained fail-closed");
    }
    const auto disableFailureState =
        DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (!disableFailureState.contextRetained
        || !disableFailureState.denyOnly
        || disableFailureState.ready) {
        return Fail("disable failure cleared live hook context or reported readiness");
    }
    failedDisable.AllowDisable();
    if (DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !failedDisable.WasRolledBack()) {
        return Fail("disable failure could not be safely retried");
    }

    FixtureHookPlatform failedRemove(FixtureHookFailures{
        .failedCreate = 5,
        .failedRemove = 2,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedRemove)
            != SaveHookInstallStatus::InstallFailed
        || failedRemove.CreatedCount() != 1) {
        return Fail("remove failure did not retain the one live created hook");
    }
    const auto removeFailureState =
        DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (!removeFailureState.contextRetained
        || !removeFailureState.denyOnly
        || removeFailureState.ready) {
        return Fail("remove failure cleared live hook context or reported readiness");
    }
    failedRemove.AllowRemove();
    if (DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !failedRemove.WasRolledBack()) {
        return Fail("remove failure could not be safely retried");
    }

    FixtureHookPlatform failedUninitialize(FixtureHookFailures{
        .missingTarget = 0,
        .failUninitialize = true,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedUninitialize)
            != SaveHookInstallStatus::InstallFailed) {
        return Fail("uninitialize failure setup did not fail installation");
    }
    const auto uninitializeFailureState =
        DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (!uninitializeFailureState.contextRetained
        || !uninitializeFailureState.denyOnly
        || uninitializeFailureState.ready) {
        return Fail("uninitialize failure cleared retained lifecycle state");
    }
    failedUninitialize.AllowUninitialize();
    if (DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !failedUninitialize.WasRolledBack()) {
        return Fail("uninitialize failure could not be safely retried");
    }

    FixtureHookPlatform teardownDisable(FixtureHookFailures{
        .failDisable = true,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, teardownDisable)
            != SaveHookInstallStatus::Success
        || DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Incomplete
        || DSRRandomizer::Save::SaveHooksAreInstalled()
        || teardownDisable.EnabledCount() != 16
        || teardownDisable.CreatedCount() != 16) {
        return Fail("ready hook cleanup did not retain state after disable failure");
    }
    const auto teardownDisableState =
        DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (!teardownDisableState.contextRetained
        || !teardownDisableState.denyOnly
        || teardownDisableState.ready) {
        return Fail("ready hook disable failure freed context or retained readiness");
    }
    teardownDisable.AllowDisable();
    if (DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !teardownDisable.WasRolledBack()) {
        return Fail("ready hook disable failure could not be safely retried");
    }

    FixtureHookPlatform teardownRemove(FixtureHookFailures{
        .failedRemove = 2,
    });
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, teardownRemove)
            != SaveHookInstallStatus::Success
        || DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Incomplete
        || teardownRemove.EnabledCount() != 0
        || teardownRemove.CreatedCount() != 1) {
        return Fail("ready hook cleanup did not retain the failed removal");
    }
    const auto teardownRemoveState =
        DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (!teardownRemoveState.contextRetained
        || !teardownRemoveState.denyOnly
        || teardownRemoveState.ready) {
        return Fail("ready hook remove failure freed context or retained readiness");
    }
    teardownRemove.AllowRemove();
    if (DSRRandomizer::Save::UninstallSaveHooks()
            != SaveHookCleanupStatus::Success
        || !teardownRemove.WasRolledBack()) {
        return Fail("ready hook remove failure could not be safely retried");
    }

    FixtureHookPlatform inFlight;
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, inFlight)
        != SaveHookInstallStatus::Success) {
        return Fail("callback quiescence setup did not install hooks");
    }
    const HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (entered == nullptr || release == nullptr) {
        if (entered != nullptr) CloseHandle(entered);
        if (release != nullptr) CloseHandle(release);
        return Fail("callback quiescence events could not be created");
    }
    std::thread callback([&] {
        DSRRandomizer::Save::Testing::HoldSaveHookCallback(entered, release);
    });
    if (WaitForSingleObject(entered, 5000) != WAIT_OBJECT_0) {
        SetEvent(release);
        callback.join();
        CloseHandle(entered);
        CloseHandle(release);
        return Fail("callback did not enter before uninstall");
    }
    auto cleanup = std::async(std::launch::async, [] {
        return DSRRandomizer::Save::UninstallSaveHooks();
    });
    if (cleanup.wait_for(std::chrono::milliseconds(100))
        != std::future_status::timeout) {
        SetEvent(release);
        callback.join();
        CloseHandle(entered);
        CloseHandle(release);
        return Fail("uninstall did not wait for the in-flight callback");
    }
    SetEvent(release);
    callback.join();
    const auto cleanupStatus = cleanup.get();
    CloseHandle(entered);
    CloseHandle(release);
    const auto finalState = DSRRandomizer::Save::Testing::CurrentSaveHookLifecycle();
    if (cleanupStatus != SaveHookCleanupStatus::Success
        || finalState.contextRetained
        || finalState.denyOnly
        || finalState.ready
        || !inFlight.WasRolledBack()) {
        return Fail("callback quiescence cleanup freed or retained the wrong state");
    }

    FixtureHookPlatform mutationBarrier;
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, mutationBarrier)
        != SaveHookInstallStatus::Success) {
        return Fail("save mutation barrier setup did not install hooks");
    }
    const HANDLE mutationCallbackEntered =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE allowMutation = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE mutationAcquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE mutationCallbackRelease =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE cleanupMutationReleased =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (mutationCallbackEntered == nullptr || allowMutation == nullptr
        || mutationAcquired == nullptr || mutationCallbackRelease == nullptr
        || cleanupMutationReleased == nullptr) {
        ExitProcess(94);
    }
    mutationBarrier.SignalMutationRelease(cleanupMutationReleased);
    std::thread mutationCallback([&]() {
        DSRRandomizer::Save::Testing::
            HoldSaveHookCallbackWhileWaitingForMutation(
                mutationCallbackEntered,
                allowMutation,
                mutationAcquired,
                mutationCallbackRelease);
    });
    if (WaitForSingleObject(mutationCallbackEntered, 5000) != WAIT_OBJECT_0) {
        ExitProcess(95);
    }
    SaveHookCleanupStatus mutationCleanupStatus =
        SaveHookCleanupStatus::Incomplete;
    std::thread mutationCleanup([&]() {
        mutationCleanupStatus = DSRRandomizer::Save::UninstallSaveHooks();
    });
    if (WaitForSingleObject(cleanupMutationReleased, 5000) != WAIT_OBJECT_0) {
        ExitProcess(96);
    }
    SetEvent(allowMutation);
    if (WaitForSingleObject(mutationAcquired, 5000) != WAIT_OBJECT_0) {
        ExitProcess(97);
    }
    SetEvent(mutationCallbackRelease);
    mutationCallback.join();
    mutationCleanup.join();
    for (const auto event : {
             mutationCallbackEntered,
             allowMutation,
             mutationAcquired,
             mutationCallbackRelease,
             cleanupMutationReleased}) {
        CloseHandle(event);
    }
    if (mutationCleanupStatus != SaveHookCleanupStatus::Success
        || !mutationBarrier.WasRolledBack()) {
        return Fail("save cleanup held mutation ownership while draining callbacks");
    }

    return 0;
}

int VerifyMissingRealRootIsAllowed(const fs::path& root) {
    auto configuration = HookConfigurationFor(root);
    configuration.realSaveRoot = (root / L"missing-real-normal").native();
    if (fs::exists(configuration.realSaveRoot)) {
        return Fail("missing real root fixture unexpectedly exists");
    }
    if (DSRRandomizer::Save::InstallSaveHooks(configuration)
        != SaveHookInstallStatus::Success) {
        return Fail("missing real root configuration was rejected");
    }
    if (DSRRandomizer::Save::UninstallSaveHooks()
        != SaveHookCleanupStatus::Success) {
        return Fail("missing real root configuration did not uninstall cleanly");
    }
    return 0;
}

int VerifyHardLinkedDedicatedIsRejected(const fs::path& root) {
    const auto testRoot = root / L"hard-linked-dedicated";
    const auto configuration = HookConfigurationFor(testRoot);
    const auto profile = testRoot / L"virtual-documents" / L"NBGI"
        / L"DARK SOULS REMASTERED" / L"12345678901234567";
    const auto hardLinkTarget = testRoot / L"real-normal" / L"normal-save.bin";
    if (!CreateDirectories(profile)
        || !CreateDirectories(testRoot / L"real-normal")
        || !CreateDirectories(testRoot / L"external")
        || !WriteFixtureFile(hardLinkTarget, "normal-save")
        || !CreateHardLinkW(
            configuration.dedicatedRmm.c_str(),
            hardLinkTarget.c_str(),
            nullptr)) {
        return Fail("hard-link fixture setup failed");
    }
    const auto installStatus = DSRRandomizer::Save::InstallSaveHooks(configuration);
    if (installStatus != SaveHookInstallStatus::InvalidConfiguration) {
        const auto cleanup = DSRRandomizer::Save::UninstallSaveHooks();
        (void)cleanup;
        std::error_code cleanupError;
        fs::remove_all(testRoot, cleanupError);
        return Fail("hard-linked dedicated save configuration was accepted");
    }
    std::error_code cleanupError;
    fs::remove_all(testRoot, cleanupError);
    if (cleanupError) {
        return Fail("hard-link fixture cleanup failed");
    }
    return 0;
}

int VerifyInspectUseSwapIsPinned(const fs::path& root) {
    const auto testRoot = root / L"inspect-use";
    const auto virtualDocuments = testRoot / L"virtual-documents";
    const auto virtualProfile = virtualDocuments
        / L"NBGI" / L"DARK SOULS REMASTERED" / L"12345678901234567";
    const auto logicalSave = virtualProfile / L"DRAKS0005.sl2";
    const auto realRoot = testRoot / L"real-normal";
    const auto externalRoot = testRoot / L"external";
    const auto savedExternalRoot = testRoot / L"saved-external";
    const auto outsideRoot = testRoot / L"outside";
    const auto dedicatedRmm = externalRoot / L"DRAKS0005.rmm";
    const auto escapedRmm = outsideRoot / L"DRAKS0005.rmm";
    if (!CreateDirectories(virtualProfile)
        || !CreateDirectories(realRoot)
        || !CreateDirectories(externalRoot)
        || !CreateDirectories(outsideRoot)
        || !WriteFixtureFile(dedicatedRmm, "approved-save")
        || !WriteFixtureFile(escapedRmm, "outside-save")) {
        return Fail("inspect/use swap fixture setup failed");
    }

    const SaveHookConfiguration configuration{
        virtualDocuments.native(),
        logicalSave.native(),
        realRoot.native(),
        externalRoot.native(),
        dedicatedRmm.native(),
        true,
        false,
    };
    if (DSRRandomizer::Save::InstallSaveHooks(configuration)
        != SaveHookInstallStatus::Success) {
        return Fail("inspect/use swap hooks did not install");
    }

    RootSwapAttempt attempt{
        dedicatedRmm.native(),
        (savedExternalRoot / L"DRAKS0005.rmm").native(),
        escapedRmm.native(),
    };
    if (!CreateDirectories(savedExternalRoot)) {
        return Fail("inspect/use saved-root fixture setup failed");
    }
    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(
        &AttemptRootSwap,
        &attempt);
    const HANDLE file = CreateFileW(
        logicalSave.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    DSRRandomizer::Save::Testing::SetBeforeOriginalApiCallback(nullptr, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (DSRRandomizer::Save::UninstallSaveHooks()
        != SaveHookCleanupStatus::Success) {
        return Fail("inspect/use swap hooks did not uninstall");
    }

    if (attempt.succeeded) {
        MoveFileW(dedicatedRmm.c_str(), escapedRmm.c_str());
        MoveFileW(
            (savedExternalRoot / L"DRAKS0005.rmm").c_str(),
            dedicatedRmm.c_str());
    }
    const bool pinWasEffective = attempt.attempted
        && !attempt.succeeded
        && file != INVALID_HANDLE_VALUE
        && fs::is_regular_file(dedicatedRmm);
    if (!pinWasEffective) {
        std::cerr << "inspect/use state: attempted=" << attempt.attempted
                  << " swapped=" << attempt.succeeded
                  << " handle=" << (file != INVALID_HANDLE_VALUE)
                  << " approved=" << fs::is_regular_file(dedicatedRmm)
                  << " outside=" << fs::exists(escapedRmm) << '\n';
    }
    std::error_code cleanupError;
    fs::remove_all(testRoot, cleanupError);
    if (!pinWasEffective) {
        return Fail("inspected save identity was not pinned through CreateFileW");
    }
    if (cleanupError) {
        return Fail("inspect/use fixture cleanup failed");
    }
    return 0;
}

int RunFixture(const wchar_t* fixturePath, const wchar_t* guardPath) {
    const TemporaryRoot root(CreateTemporaryRoot());
    if (root.Path().empty()) {
        return Fail("unable to create temporary fixture root");
    }

    const auto virtualDocuments = root.Path() / L"virtual-documents";
    const auto virtualProfile = virtualDocuments
        / L"NBGI" / L"DARK SOULS REMASTERED" / L"12345678901234567";
    const auto logicalSave = virtualProfile / L"DRAKS0005.sl2";
    const auto realSaveRoot = root.Path() / L"real-normal";
    const auto realProfile = realSaveRoot / L"12345678901234567";
    const auto realSave = realProfile / L"DRAKS0005.sl2";
    const auto realAliasTarget = realSaveRoot / L"alias-target.bin";
    const auto overhaulSave = virtualProfile / L"DRAKS0005.sl2.overhaul.sl2";
    const auto externalRoot = root.Path() / L"external";
    const auto escapeTarget = root.Path() / L"escape-target";
    const auto dedicatedRmm = externalRoot / L"DRAKS0005.rmm";
    const auto prohibitedPhysical = externalRoot
        / L"DRAKS0005.sl2.overhaul.sl2";
    if (!CreateDirectories(virtualProfile)
        || !CreateDirectories(realProfile)
        || !CreateDirectories(externalRoot)
        || !CreateDirectories(escapeTarget)
        || !WriteFixtureFile(realAliasTarget, "real-root-alias-target")
        || !WriteFixtureFile(dedicatedRmm, "preexisting-rmm")
        || !WriteFixtureFile(prohibitedPhysical, "physical-prohibited")) {
        return Fail("unable to create fixture directories");
    }
    if (const auto rollbackResult = VerifyHookInstallRollback(root.Path());
        rollbackResult != 0) {
        return rollbackResult;
    }
    if (const auto missingRootResult = VerifyMissingRealRootIsAllowed(root.Path());
        missingRootResult != 0) {
        return missingRootResult;
    }
    if (const auto hardLinkResult = VerifyHardLinkedDedicatedIsRejected(root.Path());
        hardLinkResult != 0) {
        return hardLinkResult;
    }
    if (const auto pinResult = VerifyInspectUseSwapIsPinned(root.Path());
        pinResult != 0) {
        return pinResult;
    }
    if (const auto configurationResult = VerifyGameParamConfiguration(root.Path());
        configurationResult != 0) {
        return configurationResult;
    }
    if (const auto redirectResult = VerifyGameParamRedirect(root.Path());
        redirectResult != 0) {
        return redirectResult;
    }

    const auto pipeName = L"\\\\.\\pipe\\DSRRandomizer-SaveHook-"
        + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        static_cast<DWORD>(sizeof(DSRRandomizer::ProtectionHandshakeMessage)),
        static_cast<DWORD>(sizeof(DSRRandomizer::ProtectionHandshakeMessage)),
        5000,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return Fail("unable to create fixture handshake pipe");
    }

    const HANDLE connectedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connectedEvent == nullptr) {
        CloseHandle(pipe);
        return Fail("unable to create fixture pipe event");
    }
    OVERLAPPED connection{};
    connection.hEvent = connectedEvent;
    const BOOL connectedImmediately = ConnectNamedPipe(pipe, &connection);
    const auto connectError = connectedImmediately ? ERROR_SUCCESS : GetLastError();
    const bool connectionPending = connectedImmediately
        || connectError == ERROR_IO_PENDING
        || connectError == ERROR_PIPE_CONNECTED;
    if (!connectionPending) {
        CloseHandle(connectedEvent);
        CloseHandle(pipe);
        return Fail("unable to wait for fixture pipe connection");
    }
    if (connectError == ERROR_PIPE_CONNECTED) {
        SetEvent(connectedEvent);
    }

    auto commandLine = Quote(fixturePath)
        + L" " + Quote(guardPath)
        + L" " + Quote(pipeName)
        + L" " + Quote(virtualDocuments.native())
        + L" " + Quote(logicalSave.native())
        + L" " + Quote(realSaveRoot.native())
        + L" " + Quote(overhaulSave.native())
        + L" " + Quote(externalRoot.native())
        + L" " + Quote(escapeTarget.native());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            fixturePath,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            root.Path().c_str(),
            &startup,
            &process)) {
        CancelIoEx(pipe, &connection);
        CloseHandle(connectedEvent);
        CloseHandle(pipe);
        return Fail("unable to start save fixture");
    }
    CloseHandle(process.hThread);

    const HANDLE connectionOrExit[] = {connectedEvent, process.hProcess};
    const auto connectionWait = WaitForMultipleObjects(
        static_cast<DWORD>(std::size(connectionOrExit)),
        connectionOrExit,
        FALSE,
        10000);
    bool handshakeWasValid = false;
    if (connectionWait == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        handshakeWasValid = (connectedImmediately
                || connectError == ERROR_PIPE_CONNECTED
                || GetOverlappedResult(pipe, &connection, &transferred, FALSE))
            && ReadHandshake(pipe);
    }
    else {
        CancelIoEx(pipe, &connection);
    }
    CloseHandle(connectedEvent);
    CloseHandle(pipe);

    const auto waitResult = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);

    if (!handshakeWasValid) {
        std::cerr << "save fixture exited before handshake with exit code "
                  << exitCode << '\n';
        return Fail("save fixture handshake was invalid");
    }
    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        std::cerr << "save fixture failed with exit code " << exitCode << '\n';
        return 1;
    }

    if (!fs::is_regular_file(dedicatedRmm)
        || fs::file_size(dedicatedRmm) != std::string_view("rmm-save-hook-sentinel").size()) {
        return Fail("dedicated rmm sentinel was not created");
    }
    std::error_code prohibitedRemoveError;
    if (!fs::remove(prohibitedPhysical, prohibitedRemoveError)
        || prohibitedRemoveError) {
        return Fail("unable to remove prohibited enumeration fixture");
    }
    std::error_code aliasTargetRemoveError;
    if (!fs::remove(realAliasTarget, aliasTargetRemoveError)
        || aliasTargetRemoveError) {
        return Fail("unable to remove real-root alias fixture");
    }
    if (fs::exists(logicalSave) || fs::exists(realSave) || fs::exists(overhaulSave)) {
        return Fail("fixture wrote a prohibited save path");
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root.Path())) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    if (files.size() != 1 || files.front() != dedicatedRmm) {
        return Fail("fixture wrote outside the exact dedicated rmm target");
    }

    return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        return Fail("fixture and guard paths are required");
    }
    return RunFixture(argv[1], argv[2]);
}

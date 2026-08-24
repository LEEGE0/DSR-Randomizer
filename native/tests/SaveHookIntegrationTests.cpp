#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
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

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = fs::path(temporary.data())
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
        && message.status == 0
        && message.activeFlags == 7;
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
        false,
    };
}

int VerifyHookInstallRollback(const fs::path& root) {
    const auto configuration = HookConfigurationFor(root);

    FixtureHookPlatform missingTarget(FixtureHookFailures{.missingTarget = 3});
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, missingTarget)
            != SaveHookInstallStatus::InstallFailed
        || !missingTarget.WasRolledBack()
        || missingTarget.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
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
        || failedDisable.CreatedCount() != 8) {
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
        || teardownDisable.EnabledCount() != 8
        || teardownDisable.CreatedCount() != 8) {
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
    if (const auto pinResult = VerifyInspectUseSwapIsPinned(root.Path());
        pinResult != 0) {
        return pinResult;
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

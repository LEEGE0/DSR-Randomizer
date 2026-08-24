#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "save/SaveHooks.h"

namespace {

namespace fs = std::filesystem;
using DSRRandomizer::Save::HookPlatform;
using DSRRandomizer::Save::SaveHookConfiguration;
using DSRRandomizer::Save::SaveHookInstallStatus;

class FixtureHookPlatform final : public HookPlatform {
public:
    explicit FixtureHookPlatform(
        const std::size_t missingTarget = static_cast<std::size_t>(-1),
        const std::size_t failedQueue = static_cast<std::size_t>(-1),
        const bool failApply = false)
        : missingTarget_(missingTarget),
          failedQueue_(failedQueue),
          failApply_(failApply) {}

    bool Initialize() noexcept override {
        initialized_ = true;
        return true;
    }

    void* ResolveTarget(const wchar_t*, const char*) noexcept override {
        const auto index = resolveCount_++;
        if (index == missingTarget_) {
            return nullptr;
        }
        return reinterpret_cast<void*>(0x10000ULL + (index * 0x100ULL));
    }

    bool CreateHook(void* target, void*, void** original) noexcept override {
        created_.insert(target);
        *original = target;
        return true;
    }

    bool QueueEnable(void*) noexcept override {
        return queueCount_++ != failedQueue_;
    }

    bool ApplyQueued() noexcept override {
        applyWasCalled_ = true;
        return !failApply_;
    }

    void DisableAll() noexcept override { disableWasCalled_ = true; }

    void RemoveHook(void* target) noexcept override { created_.erase(target); }

    void Uninitialize() noexcept override { initialized_ = false; }

    [[nodiscard]] bool WasRolledBack() const noexcept {
        return !initialized_ && created_.empty();
    }
    [[nodiscard]] bool ApplyWasCalled() const noexcept { return applyWasCalled_; }
    [[nodiscard]] bool DisableWasCalled() const noexcept { return disableWasCalled_; }

private:
    std::size_t missingTarget_;
    std::size_t failedQueue_;
    bool failApply_;
    std::size_t resolveCount_ = 0;
    std::size_t queueCount_ = 0;
    bool initialized_ = false;
    bool applyWasCalled_ = false;
    bool disableWasCalled_ = false;
    std::set<void*> created_;
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

    FixtureHookPlatform missingTarget(3);
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, missingTarget)
            != SaveHookInstallStatus::InstallFailed
        || !missingTarget.WasRolledBack()
        || missingTarget.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("missing hook target did not roll back the save group");
    }

    FixtureHookPlatform failedQueue(static_cast<std::size_t>(-1), 4);
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedQueue)
            != SaveHookInstallStatus::InstallFailed
        || !failedQueue.WasRolledBack()
        || failedQueue.ApplyWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("partial hook queue failure did not roll back the save group");
    }

    FixtureHookPlatform failedApply(
        static_cast<std::size_t>(-1),
        static_cast<std::size_t>(-1),
        true);
    if (DSRRandomizer::Save::InstallSaveHooks(configuration, failedApply)
            != SaveHookInstallStatus::InstallFailed
        || !failedApply.WasRolledBack()
        || !failedApply.ApplyWasCalled()
        || !failedApply.DisableWasCalled()
        || DSRRandomizer::Save::SaveHooksAreInstalled()) {
        return Fail("atomic hook apply failure did not disable and roll back the save group");
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
    const auto overhaulSave = virtualProfile / L"DRAKS0005.sl2.overhaul.sl2";
    const auto externalRoot = root.Path() / L"external";
    const auto escapeTarget = root.Path() / L"escape-target";
    const auto dedicatedRmm = externalRoot / L"DRAKS0005.rmm";
    if (!CreateDirectories(virtualProfile)
        || !CreateDirectories(realProfile)
        || !CreateDirectories(externalRoot)
        || !CreateDirectories(escapeTarget)) {
        return Fail("unable to create fixture directories");
    }
    if (const auto rollbackResult = VerifyHookInstallRollback(root.Path());
        rollbackResult != 0) {
        return rollbackResult;
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

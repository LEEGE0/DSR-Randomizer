#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "ProtectionBootstrap.h"
#include "modules/DeferredModuleGate.h"
#include "steam/SteamHooks.h"
#include "steam/SteamPolicy.h"

namespace {

using DSRRandomizer::Modules::DeferredModuleGateConfiguration;
using DSRRandomizer::Modules::DeferredModuleGateInstallStatus;
using DSRRandomizer::Modules::DeferredModuleExpectation;
using DSRRandomizer::Steam::MethodDecision;
using DSRRandomizer::Steam::SteamMethod;
using DSRRandomizer::Steam::SteamPolicy;

constexpr DWORD kWrongPathExit = 92;
constexpr DWORD kWrongHashExit = 93;
constexpr DWORD kUnsupportedInterfaceExit = 94;
constexpr DWORD kUnexpectedRawFactoryExit = 95;

using Factory = DSRRandomizer::Steam::Synthetic::FactoryFunction;
using ResetCounters = void(__cdecl*)() noexcept;
using QueryCounter = std::uint32_t(__cdecl*)() noexcept;
using SetUnexpectedFactoryExit = void(__cdecl*)(DWORD) noexcept;

const DeferredModuleGateConfiguration* activeBootstrapConfiguration = nullptr;

bool ProvideBootstrapConfiguration(
    DeferredModuleGateConfiguration& destination) noexcept {
    if (activeBootstrapConfiguration == nullptr) {
        return false;
    }
    try {
        destination = *activeBootstrapConfiguration;
        return true;
    }
    catch (...) {
        return false;
    }
}

int Fail(const char* const message) {
    std::cerr << message << '\n';
    return 1;
}

std::wstring Quote(const std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return length == 0 || length == buffer.size()
        ? std::wstring{}
        : std::wstring(buffer.data(), length);
}

std::wstring CanonicalDosPath(const std::wstring_view value) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetFullPathNameW(
        std::wstring(value).c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    return length == 0 || length >= buffer.size()
        ? std::wstring{}
        : std::wstring(buffer.data(), length);
}

std::array<std::uint8_t, 32> HashFile(const std::wstring& path) {
    std::array<std::uint8_t, 32> digest{};
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD bytes = 0;
    std::vector<std::uint8_t> object;
    bool ok = BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) >= 0
        && BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytes,
            0) >= 0;
    if (ok) {
        object.resize(objectLength);
        ok = BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0) >= 0;
    }
    std::array<std::uint8_t, 4096> buffer{};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok) {
        ok = BCryptFinishHash(
            hash,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0) >= 0;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    return ok ? digest : std::array<std::uint8_t, 32>{};
}

void FatalReporter(const char* const code) noexcept {
    DWORD exitCode = 90;
    if (code != nullptr && std::strcmp(code, "STEAM_MODULE_PATH_MISMATCH") == 0) {
        exitCode = kWrongPathExit;
    }
    else if (code != nullptr
        && std::strcmp(code, "STEAM_MODULE_HASH_MISMATCH") == 0) {
        exitCode = kWrongHashExit;
    }
    else if (code != nullptr
        && std::strcmp(code, "STEAM_INTERFACE_UNSUPPORTED") == 0) {
        exitCode = kUnsupportedInterfaceExit;
    }
    TerminateProcess(GetCurrentProcess(), exitCode);
    ExitProcess(exitCode);
}

DeferredModuleGateConfiguration Configuration(
    const std::wstring& expectedPath,
    std::array<std::uint8_t, 32> hash,
    const bool allowDeferred) {
    DeferredModuleExpectation module{};
    module.expectedPath = expectedPath;
    module.expectedSha256 = hash;
    module.allowDeferred = allowDeferred;
    module.declaredInterfaces = {
        "SteamMatchMaking009",
        "SteamNetworking006",
        "STEAMREMOTESTORAGE_INTERFACE_VERSION016",
        "SteamUser023",
    };
    module.protectedFactoryExports = {"FakeSteamFactory"};

    DeferredModuleGateConfiguration configuration{};
    configuration.modules.push_back(std::move(module));
    configuration.fatalReporter = &FatalReporter;
    return configuration;
}

template <typename Function>
Function Resolve(const HMODULE module, const char* const name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool DeniedByWrapper(void* const value) {
    if (value == nullptr) {
        return false;
    }
    const auto* const interfaceValue = static_cast<
        DSRRandomizer::Steam::Synthetic::Interface*>(value);
    return interfaceValue->vtable != nullptr
        && interfaceValue->vtable->Invoke != nullptr
        && !interfaceValue->vtable->Invoke(value);
}

int RunEager(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("could not eagerly load fake Steam module");
    }
    const auto factory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto reset = Resolve<ResetCounters>(module, "FakeSteamResetCounters");
    const auto protectedCount = Resolve<QueryCounter>(
        module,
        "FakeSteamProtectedCallCount");
    if (factory == nullptr || reset == nullptr || protectedCount == nullptr) {
        return Fail("could not resolve eager fake Steam exports");
    }
    reset();

    auto configuration = Configuration(fakePath, HashFile(fakePath), false);
    const auto installStatus =
        DSRRandomizer::Modules::Testing::
            InstallDeferredModuleGateForSyntheticSuspendedProcess(
                configuration);
    if (installStatus != DeferredModuleGateInstallStatus::Success) {
        std::cerr << "eager install status: "
                  << static_cast<int>(installStatus) << '\n';
        return Fail("eager Steam module was not admitted");
    }
    const auto guardedFactory = Resolve<Factory>(module, "FakeSteamFactory");
    if (guardedFactory == nullptr || guardedFactory == factory) {
        return Fail("GetProcAddress returned the raw protected factory address");
    }
    if (!DeniedByWrapper(factory("SteamMatchMaking009"))
        || !DeniedByWrapper(guardedFactory("SteamMatchMaking009"))
        || protectedCount() != 0) {
        return Fail("pre-resolved eager factory escaped the wrapper");
    }
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
        != DSRRandomizer::Modules::DeferredModuleGateCleanupStatus::Success) {
        return Fail("eager gate cleanup failed");
    }
    FreeLibrary(module);
    return 0;
}

int RunDeferred(const std::wstring& fakePath) {
    auto configuration = Configuration(fakePath, HashFile(fakePath), true);
    activeBootstrapConfiguration = &configuration;
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::SteamInterfaces)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::DeferredModuleGate);
    if (DSRRandomizer::Testing::InitializeWithSteamConfigurationProvider(
            &block,
            &ProvideBootstrapConfiguration) != DSRRandomizer::InitStatus::Success
        || DSRRandomizer::CurrentProtectionFlags()
            != static_cast<DSRRandomizer::ProtectionFlags>(block.requiredFlags)) {
        return Fail("deferred Steam gate was not armed");
    }
    activeBootstrapConfiguration = nullptr;
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("admitted deferred module handle was withheld");
    }
    const auto factory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto reset = Resolve<ResetCounters>(module, "FakeSteamResetCounters");
    const auto protectedCount = Resolve<QueryCounter>(
        module,
        "FakeSteamProtectedCallCount");
    const auto identityCount = Resolve<QueryCounter>(
        module,
        "FakeSteamIdentityCallCount");
    if (factory == nullptr || reset == nullptr || protectedCount == nullptr
        || identityCount == nullptr) {
        return Fail("guarded deferred exports were not resolvable");
    }
    reset();
    if (!DeniedByWrapper(factory("SteamNetworking006"))
        || !DeniedByWrapper(factory(
            "STEAMREMOTESTORAGE_INTERFACE_VERSION016"))
        || protectedCount() != 0) {
        return Fail("deferred protected interface called through raw vtable");
    }
    void* const identity = factory("SteamUser023");
    if (identity == nullptr) {
        return Fail("ownership identity interface was unavailable");
    }
    auto* const identityInterface = static_cast<
        DSRRandomizer::Steam::Synthetic::Interface*>(identity);
    if (!identityInterface->vtable->Invoke(identity) || identityCount() != 1) {
        return Fail("ownership identity call did not pass through");
    }
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
        != DSRRandomizer::Modules::DeferredModuleGateCleanupStatus::Success) {
        return Fail("deferred gate cleanup failed");
    }
    FreeLibrary(module);
    return 0;
}

int RunWrongPath(
    const std::wstring& expectedPath,
    const std::wstring& loadPath) {
    auto configuration = Configuration(
        expectedPath,
        HashFile(expectedPath),
        true);
    if (DSRRandomizer::Modules::Testing::
            InstallDeferredModuleGateForSyntheticSuspendedProcess(configuration)
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("wrong-path gate did not arm");
    }
    static_cast<void>(LoadLibraryW(loadPath.c_str()));
    return Fail("wrong-path module returned from LoadLibraryW");
}

int RunWrongHash(const std::wstring& fakePath) {
    auto hash = HashFile(fakePath);
    hash[0] ^= 0xff;
    auto configuration = Configuration(fakePath, hash, true);
    if (DSRRandomizer::Modules::Testing::
            InstallDeferredModuleGateForSyntheticSuspendedProcess(configuration)
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("wrong-hash gate did not arm");
    }
    static_cast<void>(LoadLibraryW(fakePath.c_str()));
    return Fail("wrong-hash module returned from LoadLibraryW");
}

int RunUnknownVersion(const std::wstring& fakePath) {
    auto configuration = Configuration(fakePath, HashFile(fakePath), true);
    if (DSRRandomizer::Modules::Testing::
            InstallDeferredModuleGateForSyntheticSuspendedProcess(configuration)
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("unknown-version gate did not arm");
    }
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("unknown-version fixture module did not load");
    }
    const auto factory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto setUnexpectedExit = Resolve<SetUnexpectedFactoryExit>(
        module,
        "FakeSteamSetUnexpectedFactoryExitCode");
    if (factory == nullptr || setUnexpectedExit == nullptr) {
        return Fail("unknown-version fixture exports were unavailable");
    }
    setUnexpectedExit(kUnexpectedRawFactoryExit);
    static_cast<void>(factory("SteamMatchMaking999"));
    return Fail("unknown protected interface returned from factory");
}

int VerifyPolicy() {
    const SteamPolicy policy;
    if (policy.Evaluate("SteamMatchMaking009", SteamMethod::CreateLobby)
            != MethodDecision::Deny
        || policy.Evaluate("SteamNetworking006", SteamMethod::SendP2PPacket)
            != MethodDecision::Deny
        || policy.Evaluate(
            "STEAMREMOTESTORAGE_INTERFACE_VERSION016",
            SteamMethod::FileWrite) != MethodDecision::Deny
        || policy.Evaluate("SteamUser023", SteamMethod::GetSteamID)
            != MethodDecision::Allow
        || policy.Evaluate("SteamMatchMaking999", SteamMethod::CreateLobby)
            != MethodDecision::UnknownInterfaceFatal) {
        return Fail("Steam interface policy matrix was not fail closed");
    }
    return 0;
}

DWORD RunChild(
    const std::wstring& mode,
    const std::wstring& fakePath,
    const std::wstring& loadPath = {}) {
    const auto executable = CurrentExecutablePath();
    std::wstring command = Quote(executable) + L" --child " + mode + L" "
        + Quote(fakePath);
    if (!loadPath.empty()) {
        command += L" " + Quote(loadPath);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return MAXDWORD;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = MAXDWORD;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    else {
        TerminateProcess(process.hProcess, 89);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    return exitCode;
}

std::wstring FileName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

int RunParent(const std::wstring& fakePath) {
    if (VerifyPolicy() != 0) {
        return 1;
    }
    if (RunChild(L"eager", fakePath) != 0) {
        return Fail("eager Steam guard child failed");
    }
    if (RunChild(L"deferred", fakePath) != 0) {
        return Fail("deferred Steam guard child failed");
    }

    std::array<wchar_t, MAX_PATH> temporary{};
    if (GetTempPathW(
            static_cast<DWORD>(temporary.size()),
            temporary.data()) == 0) {
        return Fail("could not resolve test temporary path");
    }
    const auto wrongDirectory = std::wstring(temporary.data())
        + L"DSRRandomizer-SteamGuard-"
        + std::to_wstring(GetCurrentProcessId());
    const auto wrongPath = wrongDirectory + L"\\" + FileName(fakePath);
    if (!CreateDirectoryW(wrongDirectory.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return Fail("could not create wrong-path fixture directory");
    }
    if (!CopyFileW(fakePath.c_str(), wrongPath.c_str(), FALSE)) {
        RemoveDirectoryW(wrongDirectory.c_str());
        return Fail("could not copy wrong-path synthetic module");
    }
    const auto wrongPathExit = RunChild(L"wrong-path", fakePath, wrongPath);
    DeleteFileW(wrongPath.c_str());
    RemoveDirectoryW(wrongDirectory.c_str());
    if (wrongPathExit != kWrongPathExit) {
        return Fail("wrong-path module did not terminate with path fatal");
    }
    if (RunChild(L"wrong-hash", fakePath) != kWrongHashExit) {
        return Fail("wrong-hash module did not terminate with hash fatal");
    }
    if (RunChild(L"unknown-version", fakePath)
        != kUnsupportedInterfaceExit) {
        return Fail("unknown interface did not terminate before raw factory");
    }
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
    if (argc == 2) {
        return RunParent(CanonicalDosPath(argv[1]));
    }
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--child") {
        const std::wstring_view mode(argv[2]);
        if (mode == L"eager") {
            return RunEager(argv[3]);
        }
        if (mode == L"deferred") {
            return RunDeferred(argv[3]);
        }
        if (mode == L"wrong-path" && argc == 5) {
            return RunWrongPath(argv[3], argv[4]);
        }
        if (mode == L"wrong-hash") {
            return RunWrongHash(argv[3]);
        }
        if (mode == L"unknown-version") {
            return RunUnknownVersion(argv[3]);
        }
    }
    return Fail("invalid SteamGuardTests arguments");
}

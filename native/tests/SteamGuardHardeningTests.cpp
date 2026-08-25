#include <Windows.h>
#include <bcrypt.h>
#include <winternl.h>

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hooks/MinHookCoordinator.h"
#include "modules/DeferredModuleGate.h"
#include "network/WinsockHooks.h"
#include "steam/SteamHooks.h"

namespace {

using DSRRandomizer::Modules::DeferredModuleGateCleanupStatus;
using DSRRandomizer::Modules::DeferredModuleGateConfiguration;
using DSRRandomizer::Modules::DeferredModuleGateInstallStatus;
using DSRRandomizer::Modules::DeferredModuleExpectation;
using Factory = DSRRandomizer::Steam::Synthetic::FactoryFunction;
using QueryCounter = std::uint32_t(__cdecl*)() noexcept;
using SetIdentityBlockEvents = void(__cdecl*)(HANDLE, HANDLE) noexcept;
using LdrLoadDllFunction = NTSTATUS(NTAPI*)(
    PWSTR,
    PULONG,
    PUNICODE_STRING,
    PHANDLE);

std::atomic<std::uint32_t> fatalCount{};
std::atomic<const char*> lastFatal{};
std::atomic<bool> denyObservedByAllocationReporter{};
std::atomic<std::size_t> slotsObservedByAllocationReporter{};

void ReturningFatalReporter(const char* const code) noexcept {
    lastFatal.store(code, std::memory_order_release);
    fatalCount.fetch_add(1, std::memory_order_acq_rel);
}

void AllocationFatalReporter(const char* const code) noexcept {
    denyObservedByAllocationReporter.store(
        DSRRandomizer::Modules::Testing::GateIsDenyOnlyForReporter(),
        std::memory_order_release);
    slotsObservedByAllocationReporter.store(
        DSRRandomizer::Modules::Testing::
            GateRetainedFactorySlotCountForReporter(),
        std::memory_order_release);
    ReturningFatalReporter(code);
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

std::wstring FileName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring TemporaryDirectory(const wchar_t* const label) {
    std::array<wchar_t, MAX_PATH> temporary{};
    if (GetTempPathW(
            static_cast<DWORD>(temporary.size()),
            temporary.data()) == 0) {
        return {};
    }
    const auto directory = std::wstring(temporary.data())
        + L"DSRRandomizer-" + label + L"-"
        + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(directory.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory;
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

DeferredModuleGateConfiguration Configuration(
    const std::wstring& path,
    const bool deferred,
    const DSRRandomizer::Steam::FatalReporter reporter =
        &ReturningFatalReporter) {
    DeferredModuleExpectation module{};
    module.expectedPath = path;
    module.expectedSha256 = HashFile(path);
    module.allowDeferred = deferred;
    module.declaredInterfaces = {
        "SteamMatchMaking009",
        "SteamNetworking006",
        "STEAMREMOTESTORAGE_INTERFACE_VERSION016",
        "SteamUser023",
    };
    module.protectedFactoryExports = {"FakeSteamFactory"};
    DeferredModuleGateConfiguration configuration{};
    configuration.modules.push_back(std::move(module));
    configuration.fatalReporter = reporter;
    return configuration;
}

DeferredModuleGateInstallStatus InstallTestGate(
    const DeferredModuleGateConfiguration& configuration) {
    return DSRRandomizer::Modules::Testing::
        InstallDeferredModuleGateForSyntheticSuspendedProcess(configuration);
}

template <typename Function>
Function Resolve(const HMODULE module, const char* const name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool InvokeSynthetic(void* const value) {
    if (value == nullptr) {
        return false;
    }
    const auto* const interfaceValue = static_cast<
        DSRRandomizer::Steam::Synthetic::Interface*>(value);
    return interfaceValue->vtable != nullptr
        && interfaceValue->vtable->Invoke != nullptr
        && interfaceValue->vtable->Invoke(value);
}

HANDLE MakeManualEvent(const bool signaled = false) {
    return CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);
}

bool WaitSignaled(const HANDLE event, const DWORD timeout = 5000) {
    return event != nullptr && WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
}

void CloseEvents(const std::initializer_list<HANDLE> events) {
    for (const auto event : events) {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }
}

int RunReturningFatal(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("returning-fatal eager fixture did not load");
    }
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto identityCount = Resolve<QueryCounter>(
        module,
        "FakeSteamIdentityCallCount");
    void* const rawIdentity = rawFactory == nullptr
        ? nullptr
        : rawFactory("SteamUser023");
    if (rawFactory == nullptr || rawIdentity == nullptr || identityCount == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("returning-fatal eager gate setup failed");
    }
    void* const identity = rawFactory("SteamUser023");
    if (identity == nullptr || identity == rawIdentity
        || !InvokeSynthetic(identity) || identityCount() != 1) {
        return Fail("identity wrapper did not forward before fatal");
    }
    const auto ordinal = GetProcAddress(module, MAKEINTRESOURCEA(1));
    const auto countAfterFatal = identityCount();
    if (ordinal != nullptr || fatalCount.load() == 0
        || rawFactory("SteamUser023") != nullptr
        || InvokeSynthetic(identity)
        || identityCount() != countAfterFatal
        || GetProcAddress(module, "FakeSteamFactory") != nullptr
        || GetProcAddress(module, "FakeSteamResetCounters") != nullptr
        || LoadLibraryW(fakePath.c_str()) != nullptr) {
        return Fail("returning reporter did not leave a permanent shared deny state");
    }
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
        != DeferredModuleGateCleanupStatus::Success) {
        return Fail("returning-fatal cleanup failed");
    }
    FreeLibrary(module);
    return 0;
}

int RunIdentityTeardown(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto setBlock = Resolve<SetIdentityBlockEvents>(
        module,
        "FakeSteamSetIdentityBlockEvents");
    const auto identityCount = Resolve<QueryCounter>(
        module,
        "FakeSteamIdentityCallCount");
    if (module == nullptr || rawFactory == nullptr || setBlock == nullptr
        || identityCount == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("identity teardown setup failed");
    }
    void* const identity = rawFactory("SteamUser023");
    const HANDLE entered = MakeManualEvent();
    const HANDLE release = MakeManualEvent();
    const HANDLE beforeFactoryDrain = MakeManualEvent();
    const HANDLE cleanupDone = MakeManualEvent();
    if (identity == nullptr || entered == nullptr || release == nullptr
        || beforeFactoryDrain == nullptr || cleanupDone == nullptr) {
        return Fail("identity teardown events were unavailable");
    }
    setBlock(entered, release);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(
        nullptr,
        beforeFactoryDrain);
    bool invocationResult = true;
    std::thread invocation([&]() { invocationResult = InvokeSynthetic(identity); });
    if (!WaitSignaled(entered)) {
        std::cerr << "identity raw entry count before timeout: "
                  << identityCount() << " fatal count: " << fatalCount.load()
                  << " code: "
                  << (lastFatal.load() == nullptr ? "(null)" : lastFatal.load())
                  << '\n';
        ExitProcess(81);
    }
    DeferredModuleGateCleanupStatus cleanupStatus =
        DeferredModuleGateCleanupStatus::Incomplete;
    std::thread cleanup([&]() {
        cleanupStatus = DSRRandomizer::Modules::UninstallDeferredModuleGate();
        SetEvent(cleanupDone);
    });
    if (!WaitSignaled(beforeFactoryDrain)) {
        ExitProcess(82);
    }
    const bool cleanupBlocked = WaitForSingleObject(cleanupDone, 0) == WAIT_TIMEOUT;
    SetEvent(release);
    invocation.join();
    cleanup.join();
    setBlock(nullptr, nullptr);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(nullptr, nullptr);
    CloseEvents({entered, release, beforeFactoryDrain, cleanupDone});
    FreeLibrary(module);
    if (!cleanupBlocked || invocationResult
        || cleanupStatus != DeferredModuleGateCleanupStatus::Success) {
        return Fail("identity invocation was not leased across teardown");
    }
    return 0;
}

int RunMutationBarrier(const std::wstring& fakePath) {
    if (InstallTestGate(Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("mutation barrier gate setup failed");
    }
    const HANDLE callbackEntered = MakeManualEvent();
    const HANDLE allowMutation = MakeManualEvent();
    const HANDLE mutationAcquired = MakeManualEvent();
    const HANDLE callbackRelease = MakeManualEvent();
    const HANDLE afterInitialDisable = MakeManualEvent();
    const HANDLE cleanupDone = MakeManualEvent();
    if (callbackEntered == nullptr || allowMutation == nullptr
        || mutationAcquired == nullptr || callbackRelease == nullptr
        || afterInitialDisable == nullptr || cleanupDone == nullptr) {
        return Fail("mutation barrier events were unavailable");
    }
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(
        afterInitialDisable,
        nullptr);
    std::thread callback([&]() {
        DSRRandomizer::Modules::Testing::
            HoldGateCallbackWhileWaitingForMutation(
                callbackEntered,
                allowMutation,
                mutationAcquired,
                callbackRelease);
    });
    if (!WaitSignaled(callbackEntered)) {
        ExitProcess(83);
    }
    DeferredModuleGateCleanupStatus cleanupStatus =
        DeferredModuleGateCleanupStatus::Incomplete;
    std::thread cleanup([&]() {
        cleanupStatus = DSRRandomizer::Modules::UninstallDeferredModuleGate();
        SetEvent(cleanupDone);
    });
    if (!WaitSignaled(afterInitialDisable)) {
        ExitProcess(84);
    }
    SetEvent(allowMutation);
    if (!WaitSignaled(mutationAcquired)) {
        ExitProcess(85);
    }
    SetEvent(callbackRelease);
    callback.join();
    cleanup.join();
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(nullptr, nullptr);
    const bool completed = WaitSignaled(cleanupDone, 0)
        && cleanupStatus == DeferredModuleGateCleanupStatus::Success;
    CloseEvents({
        callbackEntered,
        allowMutation,
        mutationAcquired,
        callbackRelease,
        afterInitialDisable,
        cleanupDone,
    });
    return completed
        ? 0
        : Fail("cleanup held mutation ownership while draining callbacks");
}

int RunPostCreateFailure(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("post-create fixture did not load");
    }
    DSRRandomizer::Modules::Testing::FailNextFactoryPostCreateBookkeeping();
    DSRRandomizer::Hooks::Testing::SetMinHookFaults({0, 0, 64});
    const auto install = InstallTestGate(Configuration(
        fakePath,
        false,
        &AllocationFatalReporter));
    const auto retained = DSRRandomizer::Modules::Testing::CurrentGateLifecycle();
    const bool safelyRetained = install
            == DeferredModuleGateInstallStatus::AdmissionFailed
        && denyObservedByAllocationReporter.load(std::memory_order_acquire)
        && slotsObservedByAllocationReporter.load(std::memory_order_acquire) != 0
        && retained.contextRetained && retained.denyOnly
        && retained.factorySlotsRetained != 0;
    DSRRandomizer::Hooks::Testing::SetMinHookFaults({});
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return safelyRetained && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("post-create failure orphaned hook state or reported before deny");
}

int RunUnhookedLoaderCallout(
    const std::wstring& fakePath,
    const std::wstring& nestedPath) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ldrLoadDll = Resolve<LdrLoadDllFunction>(ntdll, "LdrLoadDll");
    if (ldrLoadDll == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM",
            fakePath.c_str())) {
        return Fail("unhooked loader-callout setup failed");
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(nestedPath.c_str());
    name.Length = static_cast<USHORT>(nestedPath.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length + sizeof(wchar_t);
    HANDLE loaded = nullptr;
    static_cast<void>(ldrLoadDll(nullptr, nullptr, &name, &loaded));
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM", nullptr);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr
        && std::strcmp(code, "STEAM_LOADER_CALLOUT_LOAD_DENIED") == 0
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr
        && LoadLibraryW(fakePath.c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    if (loaded != nullptr) {
        FreeLibrary(static_cast<HMODULE>(loaded));
    }
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("unhooked loader callout was mistaken for loader-lock-safe entry");
}

int RunNestedUnprotectedBridge(
    const std::wstring& fakePath,
    const std::wstring& nestedPath,
    const std::wstring& bridgePath) {
    if (InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM",
            bridgePath.c_str())
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_BRIDGE_TARGET",
            fakePath.c_str())) {
        return Fail("nested unprotected bridge setup failed");
    }
    const HMODULE outer = LoadLibraryW(nestedPath.c_str());
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM", nullptr);
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_BRIDGE_TARGET", nullptr);
    const bool denied = outer == nullptr
        && fatalCount.load(std::memory_order_acquire) != 0
        && GetModuleHandleW(FileName(bridgePath).c_str()) == nullptr
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("nested unprotected bridge published before protected scan");
}

int RunStaticProtectedDependency(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    if (InstallTestGate(Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("static protected dependency setup failed");
    }
    const HMODULE carrier = LoadLibraryW(carrierPath.c_str());
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool deniedBeforePublication = carrier == nullptr
        && fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr
        && std::strcmp(code, "STEAM_PROTECTED_IMPORT_PREFLIGHT_DENIED") == 0
        && GetModuleHandleW(FileName(carrierPath).c_str()) == nullptr
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return deniedBeforePublication
            && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("static protected dependency executed before gate admission");
}

int RunStaticProtectedImportClosure(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    const auto slash = carrierPath.find_last_of(L"\\/");
    const auto outerPath = (slash == std::wstring::npos
            ? std::wstring{}
            : carrierPath.substr(0, slash + 1))
        + L"DSRRandomizer.StaticSteamOuter.dll";
    if (InstallTestGate(Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("static protected import closure setup failed");
    }
    const HMODULE outer = LoadLibraryW(outerPath.c_str());
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool deniedBeforePublication = outer == nullptr
        && fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr
        && std::strcmp(code, "STEAM_DEPENDENCY_CLOSURE_UNAVAILABLE") == 0
        && GetModuleHandleW(FileName(outerPath).c_str()) == nullptr
        && GetModuleHandleW(FileName(carrierPath).c_str()) == nullptr
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return deniedBeforePublication
            && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("nested static protected import closure executed before gate admission");
}

int RunDelayProtectedDependency(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    const auto slash = carrierPath.find_last_of(L"\\/");
    const auto delayPath = (slash == std::wstring::npos
            ? std::wstring{}
            : carrierPath.substr(0, slash + 1))
        + L"DSRRandomizer.DelaySteamCarrier.dll";
    if (InstallTestGate(Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("delay protected dependency setup failed");
    }
    const HMODULE carrier = LoadLibraryW(delayPath.c_str());
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool deniedBeforePublication = carrier == nullptr
        && fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr
        && std::strcmp(code, "STEAM_PROTECTED_IMPORT_PREFLIGHT_DENIED") == 0
        && GetModuleHandleW(FileName(delayPath).c_str()) == nullptr
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return deniedBeforePublication
            && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("delay protected dependency executed before gate admission");
}

int RunEagerReplacement(const std::wstring& fakePath) {
    const auto directory = TemporaryDirectory(L"SteamEagerReplacement");
    const auto expected = directory + L"\\replacement.dll";
    const auto moved = directory + L"\\mapped-old.dll";
    if (directory.empty() || !CopyFileW(fakePath.c_str(), expected.c_str(), FALSE)) {
        return Fail("could not stage eager replacement fixture");
    }
    const HMODULE module = LoadLibraryW(expected.c_str());
    if (module == nullptr
        || !MoveFileW(expected.c_str(), moved.c_str())
        || !CopyFileW(fakePath.c_str(), expected.c_str(), FALSE)) {
        return Fail("could not establish eager replacement race");
    }
    const auto status = InstallTestGate(Configuration(expected, false));
    const bool rejected = status == DeferredModuleGateInstallStatus::AdmissionFailed
        && fatalCount.load() != 0;
    static_cast<void>(DSRRandomizer::Modules::UninstallDeferredModuleGate());
    FreeLibrary(module);
    DeleteFileW(expected.c_str());
    DeleteFileW(moved.c_str());
    RemoveDirectoryW(directory.c_str());
    return rejected ? 0 : Fail("mapped eager replacement was admitted by pathname");
}

int RunDeferredReplacement(const std::wstring& fakePath) {
    const auto directory = TemporaryDirectory(L"SteamDeferredReplacement");
    const auto expected = directory + L"\\deferred.dll";
    const auto moved = directory + L"\\moved.dll";
    if (directory.empty() || !CopyFileW(fakePath.c_str(), expected.c_str(), FALSE)
        || InstallTestGate(Configuration(expected, true))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("could not arm deferred replacement fixture");
    }
    SetLastError(ERROR_SUCCESS);
    const bool renamed = MoveFileW(expected.c_str(), moved.c_str()) != FALSE;
    const DWORD renameError = GetLastError();
    const HMODULE loaded = LoadLibraryW(expected.c_str());
    const bool protectedLoad = loaded != nullptr;
    if (loaded != nullptr) {
        FreeLibrary(loaded);
    }
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    DeleteFileW(expected.c_str());
    DeleteFileW(moved.c_str());
    RemoveDirectoryW(directory.c_str());
    if (renamed || renameError != ERROR_SHARING_VIOLATION || !protectedLoad
        || cleanup != DeferredModuleGateCleanupStatus::Success) {
        return Fail("pinned deferred expectation allowed replacement race");
    }
    return 0;
}

int RunDuplicateBasename(const std::wstring& fakePath) {
    const auto directory = TemporaryDirectory(L"SteamDuplicateBasename");
    const auto firstDirectory = directory + L"\\one";
    const auto secondDirectory = directory + L"\\two";
    CreateDirectoryW(firstDirectory.c_str(), nullptr);
    CreateDirectoryW(secondDirectory.c_str(), nullptr);
    const auto first = firstDirectory + L"\\duplicate.dll";
    const auto second = secondDirectory + L"\\duplicate.dll";
    if (!CopyFileW(fakePath.c_str(), first.c_str(), FALSE)
        || !CopyFileW(fakePath.c_str(), second.c_str(), FALSE)) {
        return Fail("could not stage duplicate basename modules");
    }
    const HMODULE firstModule = LoadLibraryW(first.c_str());
    const HMODULE secondModule = LoadLibraryW(second.c_str());
    if (firstModule == nullptr || secondModule == nullptr
        || firstModule == secondModule) {
        return Fail("Windows did not create distinct duplicate-basename fixtures");
    }
    const auto status = InstallTestGate(Configuration(first, false));
    const bool rejected = status == DeferredModuleGateInstallStatus::AdmissionFailed
        && fatalCount.load() != 0;
    static_cast<void>(DSRRandomizer::Modules::UninstallDeferredModuleGate());
    FreeLibrary(secondModule);
    FreeLibrary(firstModule);
    DeleteFileW(second.c_str());
    DeleteFileW(first.c_str());
    RemoveDirectoryW(secondDirectory.c_str());
    RemoveDirectoryW(firstDirectory.c_str());
    RemoveDirectoryW(directory.c_str());
    return rejected ? 0 : Fail("duplicate protected basenames were not rejected");
}

int RunUnloadReuse(const std::wstring& fakePath) {
    HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("unload fixture did not load");
    }
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    if (rawFactory == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("unload fixture did not admit");
    }
    FreeLibrary(module);
    const HMODULE pinned = GetModuleHandleW(FileName(fakePath).c_str());
    const HMODULE reloaded = LoadLibraryW(fakePath.c_str());
    if (pinned == nullptr || reloaded != pinned
        || rawFactory("SteamMatchMaking009") == nullptr) {
        return Fail("admitted module was unloadable or handle identity changed");
    }
    FreeLibrary(reloaded);
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
        != DeferredModuleGateCleanupStatus::Success) {
        return Fail("unload fixture cleanup failed");
    }
    return 0;
}

int RunNestedLoader(
    const std::wstring& fakePath,
    const std::wstring& nestedPath) {
    if (InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM",
            fakePath.c_str())) {
        return Fail("nested loader gate setup failed");
    }
    const HMODULE nested = LoadLibraryW(nestedPath.c_str());
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_NESTED_STEAM", nullptr);
    const bool denied = nested == nullptr && fatalCount.load() != 0
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr
        && LoadLibraryW(fakePath.c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("nested DllMain protected load was not denied before publication");
}

int RunConcurrentGroups(const std::wstring& fakePath) {
    DSRRandomizer::Network::WinsockHookConfiguration winsock{};
    winsock.endpointCount = 1;
    winsock.endpoints[0].transport = DSRRandomizer::SocketTransport::Tcp;
    winsock.endpoints[0].family = AF_INET;
    winsock.endpoints[0].port = 1;
    winsock.endpoints[0].address[0] = 127;
    winsock.endpoints[0].address[3] = 1;

    std::barrier start(3);
    DeferredModuleGateInstallStatus gateStatus{};
    DSRRandomizer::Network::WinsockHookInstallStatus winsockStatus{};
    std::thread gateThread([&]() {
        start.arrive_and_wait();
        gateStatus = InstallTestGate(Configuration(fakePath, true));
    });
    std::thread winsockThread([&]() {
        start.arrive_and_wait();
        winsockStatus = DSRRandomizer::Network::InstallWinsockHooks(winsock);
    });
    start.arrive_and_wait();
    gateThread.join();
    winsockThread.join();
    if (gateStatus != DeferredModuleGateInstallStatus::Success
        || winsockStatus
            != DSRRandomizer::Network::WinsockHookInstallStatus::Success
        || DSRRandomizer::Hooks::Testing::MinHookReferenceCount() != 2) {
        return Fail("concurrent hook families did not share MinHook ownership");
    }
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
            != DeferredModuleGateCleanupStatus::Success
        || DSRRandomizer::Hooks::Testing::MinHookReferenceCount() != 1
        || DSRRandomizer::Network::UninstallWinsockHooks()
            != DSRRandomizer::Network::WinsockHookCleanupStatus::Success
        || DSRRandomizer::Hooks::Testing::MinHookReferenceCount() != 0) {
        return Fail("shared MinHook ownership was released beneath a survivor");
    }
    return 0;
}

int RunRollbackFailure(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr) {
        return Fail("rollback fixture did not load");
    }
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    DSRRandomizer::Modules::Testing::FailNextFactoryPublication();
    DSRRandomizer::Hooks::Testing::SetMinHookFaults({0, 64, 64});
    const auto install = InstallTestGate(Configuration(fakePath, false));
    const auto retained = DSRRandomizer::Modules::Testing::CurrentGateLifecycle();
    if (install == DeferredModuleGateInstallStatus::Success
        || rawFactory == nullptr
        || rawFactory("SteamMatchMaking009") != nullptr
        || !retained.contextRetained || retained.factorySlotsRetained == 0
        || DSRRandomizer::Hooks::Testing::MinHookReferenceCount() != 1
        || DSRRandomizer::Modules::UninstallDeferredModuleGate()
            != DeferredModuleGateCleanupStatus::Incomplete) {
        return Fail("failed factory rollback recycled a live detour or ownership");
    }
    DSRRandomizer::Hooks::Testing::SetMinHookFaults({});
    if (DSRRandomizer::Modules::UninstallDeferredModuleGate()
            != DeferredModuleGateCleanupStatus::Success
        || DSRRandomizer::Hooks::Testing::MinHookReferenceCount() != 0) {
        return Fail("retained rollback state could not be cleaned up safely");
    }
    FreeLibrary(module);
    return 0;
}

int RunProductionInvariant(const std::wstring& fakePath) {
    return DSRRandomizer::Modules::InstallDeferredModuleGate(
               Configuration(fakePath, true))
            == DeferredModuleGateInstallStatus::InvalidConfiguration
        ? 0
        : Fail("production API accepted an unproven suspended invariant");
}

DWORD RunChild(
    const std::wstring& mode,
    const std::wstring& fakePath,
    const std::wstring& nestedPath,
    const std::wstring& bridgePath,
    const std::wstring& carrierPath) {
    auto command = Quote(CurrentExecutablePath()) + L" --child " + mode
        + L" " + Quote(fakePath) + L" " + Quote(nestedPath)
        + L" " + Quote(bridgePath) + L" " + Quote(carrierPath);
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
    const auto wait = WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = MAXDWORD;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    else {
        TerminateProcess(process.hProcess, 88);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    return exitCode;
}

int RunParent(
    const std::wstring& fakePath,
    const std::wstring& nestedPath,
    const std::wstring& bridgePath,
    const std::wstring& carrierPath) {
    constexpr std::array modes{
        L"returning-fatal",
        L"identity-teardown",
        L"mutation-barrier",
        L"post-create-failure",
        L"eager-replacement",
        L"deferred-replacement",
        L"duplicate-basename",
        L"unload-reuse",
        L"nested-loader",
        L"unhooked-loader-callout",
        L"nested-unprotected-bridge",
        L"static-protected-dependency",
        L"static-protected-import-closure",
        L"delay-protected-dependency",
        L"concurrent-groups",
        L"rollback-failure",
        L"production-invariant",
    };
    for (const auto* const mode : modes) {
        const auto exit = RunChild(
            mode,
            fakePath,
            nestedPath,
            bridgePath,
            carrierPath);
        if (exit != 0) {
            std::wcerr << L"hardening child failed: " << mode
                       << L" exit=" << exit << L'\n';
            return 1;
        }
    }
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
    if (argc == 5) {
        return RunParent(
            CanonicalDosPath(argv[1]),
            CanonicalDosPath(argv[2]),
            CanonicalDosPath(argv[3]),
            CanonicalDosPath(argv[4]));
    }
    if (argc == 7 && std::wstring_view(argv[1]) == L"--child") {
        const std::wstring_view mode(argv[2]);
        const std::wstring fakePath = CanonicalDosPath(argv[3]);
        const std::wstring nestedPath = CanonicalDosPath(argv[4]);
        const std::wstring bridgePath = CanonicalDosPath(argv[5]);
        const std::wstring carrierPath = CanonicalDosPath(argv[6]);
        if (mode == L"returning-fatal") {
            return RunReturningFatal(fakePath);
        }
        if (mode == L"identity-teardown") {
            return RunIdentityTeardown(fakePath);
        }
        if (mode == L"mutation-barrier") {
            return RunMutationBarrier(fakePath);
        }
        if (mode == L"post-create-failure") {
            return RunPostCreateFailure(fakePath);
        }
        if (mode == L"eager-replacement") {
            return RunEagerReplacement(fakePath);
        }
        if (mode == L"deferred-replacement") {
            return RunDeferredReplacement(fakePath);
        }
        if (mode == L"duplicate-basename") {
            return RunDuplicateBasename(fakePath);
        }
        if (mode == L"unload-reuse") {
            return RunUnloadReuse(fakePath);
        }
        if (mode == L"nested-loader") {
            return RunNestedLoader(fakePath, nestedPath);
        }
        if (mode == L"unhooked-loader-callout") {
            return RunUnhookedLoaderCallout(fakePath, nestedPath);
        }
        if (mode == L"nested-unprotected-bridge") {
            return RunNestedUnprotectedBridge(
                fakePath,
                nestedPath,
                bridgePath);
        }
        if (mode == L"static-protected-dependency") {
            return RunStaticProtectedDependency(fakePath, carrierPath);
        }
        if (mode == L"static-protected-import-closure") {
            return RunStaticProtectedImportClosure(fakePath, carrierPath);
        }
        if (mode == L"delay-protected-dependency") {
            return RunDelayProtectedDependency(fakePath, carrierPath);
        }
        if (mode == L"concurrent-groups") {
            return RunConcurrentGroups(fakePath);
        }
        if (mode == L"rollback-failure") {
            return RunRollbackFailure(fakePath);
        }
        if (mode == L"production-invariant") {
            return RunProductionInvariant(fakePath);
        }
    }
    return Fail("invalid SteamGuardHardeningTests arguments");
}

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
using ResetCounters = void(__cdecl*)() noexcept;
using QueryCounter = std::uint32_t(__cdecl*)() noexcept;
using SetIdentityBlockEvents = void(__cdecl*)(HANDLE, HANDLE) noexcept;
using SetFactoryBlockEvents = void(__cdecl*)(HANDLE, HANDLE) noexcept;
using LdrLoadDllFunction = NTSTATUS(NTAPI*)(
    PWSTR,
    PULONG,
    PUNICODE_STRING,
    PHANDLE);
using LdrGetProcedureAddressFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*);
using LdrGetProcedureAddressExFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*,
    ULONG);
using LdrGetProcedureAddressForCallerFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*,
    ULONG,
    PVOID);

std::atomic<std::uint32_t> fatalCount{};
std::atomic<const char*> lastFatal{};
std::atomic<bool> denyObservedByAllocationReporter{};
std::atomic<std::size_t> slotsObservedByAllocationReporter{};
Factory eagerFactoryForReporter = nullptr;
QueryCounter factoryCountForReporter = nullptr;
std::atomic<void*> factoryResultObservedByReporter{};
std::atomic<std::uint32_t> rawFactoryCallsObservedByReporter{};
std::atomic<void*> countedMetadataPage{};
std::atomic<SIZE_T> countedMetadataPageSize{};
std::atomic<DWORD> countedMetadataOldProtection{};

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

void AppliedFactoryFatalReporter(const char* const code) noexcept {
    void* const result = eagerFactoryForReporter == nullptr
        ? reinterpret_cast<void*>(1)
        : eagerFactoryForReporter("SteamMatchMaking009");
    factoryResultObservedByReporter.store(result, std::memory_order_release);
    rawFactoryCallsObservedByReporter.store(
        factoryCountForReporter == nullptr ? UINT32_MAX : factoryCountForReporter(),
        std::memory_order_release);
    ReturningFatalReporter(code);
}

void InvalidateCountedMetadataAfterSnapshot() noexcept {
    const auto page = countedMetadataPage.load(std::memory_order_acquire);
    const auto size = countedMetadataPageSize.load(std::memory_order_acquire);
    DWORD oldProtection = 0;
    if (page != nullptr && size != 0) {
        auto* const metadata = static_cast<ANSI_STRING*>(page);
        metadata->Buffer = reinterpret_cast<PCHAR>(1);
        metadata->Length = USHRT_MAX;
        metadata->MaximumLength = 0;
    }
    if (page != nullptr && size != 0
        && VirtualProtect(page, size, PAGE_NOACCESS, &oldProtection)) {
        countedMetadataOldProtection.store(
            oldProtection,
            std::memory_order_release);
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

DeferredModuleGateConfiguration ProtectedOuterConfiguration(
    const std::wstring& path,
    const std::string& factoryExport) {
    auto configuration = Configuration(path, true);
    configuration.modules.front().protectedFactoryExports = {factoryExport};
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
        || identityCount == nullptr) {
        return Fail("identity teardown setup failed");
    }
    void* identity = nullptr;
    const HANDLE entered = MakeManualEvent();
    const HANDLE release = MakeManualEvent();
    const HANDLE beforeFactoryDrain = MakeManualEvent();
    const HANDLE cleanupDone = MakeManualEvent();
    const HANDLE invocationStart = MakeManualEvent();
    const HANDLE cleanupStart = MakeManualEvent();
    if (entered == nullptr || release == nullptr
        || beforeFactoryDrain == nullptr || cleanupDone == nullptr) {
        return Fail("identity teardown events were unavailable");
    }
    bool invocationResult = true;
    DeferredModuleGateCleanupStatus cleanupStatus =
        DeferredModuleGateCleanupStatus::Incomplete;
    std::thread invocation([&]() {
        WaitForSingleObject(invocationStart, INFINITE);
        invocationResult = identity != nullptr && InvokeSynthetic(identity);
    });
    std::thread cleanup([&]() {
        WaitForSingleObject(cleanupStart, INFINITE);
        cleanupStatus = DSRRandomizer::Modules::UninstallDeferredModuleGate();
        SetEvent(cleanupDone);
    });
    if (invocationStart == nullptr || cleanupStart == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        SetEvent(invocationStart);
        SetEvent(cleanupStart);
        invocation.join();
        cleanup.join();
        return Fail("identity teardown gate setup failed");
    }
    identity = rawFactory("SteamUser023");
    if (identity == nullptr) {
        ExitProcess(80);
    }
    setBlock(entered, release);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(
        nullptr,
        beforeFactoryDrain);
    SetEvent(invocationStart);
    if (!WaitSignaled(entered)) {
        std::cerr << "identity raw entry count before timeout: "
                  << identityCount() << " fatal count: " << fatalCount.load()
                  << " code: "
                  << (lastFatal.load() == nullptr ? "(null)" : lastFatal.load())
                  << '\n';
        ExitProcess(81);
    }
    SetEvent(cleanupStart);
    if (!WaitSignaled(beforeFactoryDrain)) {
        ExitProcess(82);
    }
    const bool cleanupBlocked = WaitForSingleObject(cleanupDone, 0) == WAIT_TIMEOUT;
    SetEvent(release);
    invocation.join();
    cleanup.join();
    setBlock(nullptr, nullptr);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(nullptr, nullptr);
    CloseEvents({
        entered,
        release,
        beforeFactoryDrain,
        cleanupDone,
        invocationStart,
        cleanupStart,
    });
    FreeLibrary(module);
    if (!cleanupBlocked || invocationResult
        || cleanupStatus != DeferredModuleGateCleanupStatus::Success) {
        return Fail("identity invocation was not leased across teardown");
    }
    return 0;
}

int RunMutationBarrier(const std::wstring& fakePath) {
    const HANDLE callbackEntered = MakeManualEvent();
    const HANDLE allowMutation = MakeManualEvent();
    const HANDLE mutationAcquired = MakeManualEvent();
    const HANDLE callbackRelease = MakeManualEvent();
    const HANDLE afterInitialDisable = MakeManualEvent();
    const HANDLE cleanupDone = MakeManualEvent();
    const HANDLE callbackStart = MakeManualEvent();
    const HANDLE cleanupStart = MakeManualEvent();
    if (callbackEntered == nullptr || allowMutation == nullptr
        || mutationAcquired == nullptr || callbackRelease == nullptr
        || afterInitialDisable == nullptr || cleanupDone == nullptr
        || callbackStart == nullptr || cleanupStart == nullptr) {
        return Fail("mutation barrier events were unavailable");
    }
    std::thread callback([&]() {
        WaitForSingleObject(callbackStart, INFINITE);
        DSRRandomizer::Modules::Testing::
            HoldGateCallbackWhileWaitingForMutation(
                callbackEntered,
                allowMutation,
                mutationAcquired,
                callbackRelease);
    });
    DeferredModuleGateCleanupStatus cleanupStatus =
        DeferredModuleGateCleanupStatus::Incomplete;
    std::thread cleanup([&]() {
        WaitForSingleObject(cleanupStart, INFINITE);
        cleanupStatus = DSRRandomizer::Modules::UninstallDeferredModuleGate();
        SetEvent(cleanupDone);
    });
    if (InstallTestGate(Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        SetEvent(callbackStart);
        SetEvent(allowMutation);
        SetEvent(callbackRelease);
        SetEvent(cleanupStart);
        callback.join();
        cleanup.join();
        return Fail("mutation barrier gate setup failed");
    }
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(
        afterInitialDisable,
        nullptr);
    SetEvent(callbackStart);
    if (!WaitSignaled(callbackEntered)) {
        ExitProcess(83);
    }
    SetEvent(cleanupStart);
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
        callbackStart,
        cleanupStart,
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

int RunAppliedFactoryReporterRetention(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto reset = Resolve<ResetCounters>(module, "FakeSteamResetCounters");
    const auto factoryCount = Resolve<QueryCounter>(
        module,
        "FakeSteamFactoryCallCount");
    if (module == nullptr || rawFactory == nullptr || reset == nullptr
        || factoryCount == nullptr) {
        return Fail("applied factory reporter setup failed");
    }
    reset();
    eagerFactoryForReporter = rawFactory;
    factoryCountForReporter = factoryCount;
    factoryResultObservedByReporter.store(
        reinterpret_cast<void*>(1),
        std::memory_order_release);
    rawFactoryCallsObservedByReporter.store(UINT32_MAX, std::memory_order_release);
    DSRRandomizer::Modules::Testing::FailNextFactoryPublication();
    const auto install = InstallTestGate(Configuration(
        fakePath,
        false,
        &AppliedFactoryFatalReporter));
    const auto retained = DSRRandomizer::Modules::Testing::CurrentGateLifecycle();
    const bool deniedThroughReporter = install
            == DeferredModuleGateInstallStatus::AdmissionFailed
        && factoryResultObservedByReporter.load(std::memory_order_acquire) == nullptr
        && rawFactoryCallsObservedByReporter.load(std::memory_order_acquire) == 0
        && factoryCount() == 0
        && retained.contextRetained && retained.denyOnly
        && retained.factorySlotsRetained != 0
        && rawFactory("SteamMatchMaking009") == nullptr
        && factoryCount() == 0;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    const bool rawRestoredAfterCleanup = rawFactory("SteamMatchMaking009") != nullptr
        && factoryCount() == 1;
    eagerFactoryForReporter = nullptr;
    factoryCountForReporter = nullptr;
    FreeLibrary(module);
    return deniedThroughReporter
            && cleanup == DeferredModuleGateCleanupStatus::Success
            && rawRestoredAfterCleanup
        ? 0
        : Fail("applied factory escaped during reporter or failure handoff");
}

int RunActiveFactoryRollback(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto setFactoryBlock = Resolve<SetFactoryBlockEvents>(
        module,
        "FakeSteamSetFactoryBlockEvents");
    const HANDLE afterApply = MakeManualEvent();
    const HANDLE allowRollback = MakeManualEvent();
    const HANDLE factoryEntered = MakeManualEvent();
    const HANDLE factoryRelease = MakeManualEvent();
    const HANDLE afterInitialDisable = MakeManualEvent();
    const HANDLE beforeFactoryDrain = MakeManualEvent();
    const HANDLE installDone = MakeManualEvent();
    const HANDLE cleanupDone = MakeManualEvent();
    const HANDLE installStart = MakeManualEvent();
    const HANDLE cleanupStart = MakeManualEvent();
    const HANDLE callbackStart = MakeManualEvent();
    if (module == nullptr || rawFactory == nullptr || setFactoryBlock == nullptr
        || afterApply == nullptr || allowRollback == nullptr
        || factoryEntered == nullptr || factoryRelease == nullptr
        || afterInitialDisable == nullptr || beforeFactoryDrain == nullptr
        || installDone == nullptr
        || cleanupDone == nullptr || installStart == nullptr
        || cleanupStart == nullptr || callbackStart == nullptr) {
        return Fail("active factory rollback setup failed");
    }
    setFactoryBlock(factoryEntered, factoryRelease);
    DSRRandomizer::Modules::Testing::SetFactoryPublicationPauseEvents(
        afterApply,
        allowRollback);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(
        afterInitialDisable,
        beforeFactoryDrain);
    DSRRandomizer::Modules::Testing::FailNextFactoryPublication();
    DeferredModuleGateInstallStatus installStatus =
        DeferredModuleGateInstallStatus::Success;
    DeferredModuleGateCleanupStatus cleanupStatus =
        DeferredModuleGateCleanupStatus::Incomplete;
    void* callbackResult = reinterpret_cast<void*>(1);
    std::thread callback([&]() {
        WaitForSingleObject(callbackStart, INFINITE);
        callbackResult = rawFactory("SteamUser023");
    });
    std::thread install([&]() {
        WaitForSingleObject(installStart, INFINITE);
        installStatus = InstallTestGate(Configuration(fakePath, false));
        SetEvent(installDone);
    });
    std::thread cleanup([&]() {
        WaitForSingleObject(cleanupStart, INFINITE);
        cleanupStatus = DSRRandomizer::Modules::UninstallDeferredModuleGate();
        SetEvent(cleanupDone);
    });
    SetEvent(installStart);
    if (!WaitSignaled(afterApply)) {
        ExitProcess(102);
    }
    SetEvent(callbackStart);
    if (!WaitSignaled(factoryEntered)) {
        ExitProcess(103);
    }
    FreeLibrary(module);
    SetEvent(allowRollback);
    if (!WaitSignaled(installDone)) {
        ExitProcess(104);
    }
    const auto retained = DSRRandomizer::Modules::Testing::CurrentGateLifecycle();
    const bool noImplicitDrain =
        WaitForSingleObject(afterInitialDisable, 0) == WAIT_TIMEOUT
        && WaitForSingleObject(beforeFactoryDrain, 0) == WAIT_TIMEOUT;
    SetEvent(factoryRelease);
    callback.join();
    install.join();
    SetEvent(cleanupStart);
    if (!WaitSignaled(afterInitialDisable)) {
        ExitProcess(105);
    }
    if (!WaitSignaled(beforeFactoryDrain)) {
        ExitProcess(106);
    }
    if (!WaitSignaled(cleanupDone)) {
        ExitProcess(107);
    }
    cleanup.join();
    DSRRandomizer::Modules::Testing::SetFactoryPublicationPauseEvents(
        nullptr,
        nullptr);
    DSRRandomizer::Modules::Testing::SetGateCleanupPhaseEvents(nullptr, nullptr);
    const bool unloaded = GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    CloseEvents({
        afterApply,
        allowRollback,
        factoryEntered,
        factoryRelease,
        afterInitialDisable,
        beforeFactoryDrain,
        installDone,
        cleanupDone,
        installStart,
        cleanupStart,
        callbackStart,
    });
    return noImplicitDrain && callbackResult == nullptr
            && retained.contextRetained && retained.denyOnly
            && retained.factorySlotsRetained != 0 && unloaded
            && installStatus == DeferredModuleGateInstallStatus::AdmissionFailed
            && cleanupStatus == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("explicit factory cleanup did not retain and drain active state");
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

int RunProtectedOuterDependency(
    const std::wstring& fakePath,
    const std::wstring& outerPath,
    const std::string& factoryExport,
    const std::initializer_list<std::wstring> absentModules) {
    if (InstallTestGate(ProtectedOuterConfiguration(outerPath, factoryExport))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("protected outer dependency setup failed");
    }
    const HMODULE outer = LoadLibraryW(outerPath.c_str());
    const auto code = lastFatal.load(std::memory_order_acquire);
    bool absent = outer == nullptr
        && fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr
        && std::strcmp(code, "STEAM_DEPENDENCY_CLOSURE_UNAVAILABLE") == 0
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    for (const auto& module : absentModules) {
        absent = absent
            && GetModuleHandleW(FileName(module).c_str()) == nullptr;
    }
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return absent && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("protected outer dependency executed before preflight denial");
}

int RunProtectedOuterDirect(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    return RunProtectedOuterDependency(
        fakePath,
        carrierPath,
        "StaticSteamCarrierFactory",
        {carrierPath});
}

int RunProtectedOuterIntermediate(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    const auto slash = carrierPath.find_last_of(L"\\/");
    const auto outerPath = carrierPath.substr(0, slash + 1)
        + L"DSRRandomizer.StaticSteamOuter.dll";
    return RunProtectedOuterDependency(
        fakePath,
        outerPath,
        "StaticSteamOuterFactory",
        {outerPath, carrierPath});
}

int RunProtectedOuterDelay(
    const std::wstring& fakePath,
    const std::wstring& carrierPath) {
    const auto slash = carrierPath.find_last_of(L"\\/");
    const auto delayPath = carrierPath.substr(0, slash + 1)
        + L"DSRRandomizer.DelaySteamCarrier.dll";
    return RunProtectedOuterDependency(
        fakePath,
        delayPath,
        "DelaySteamCarrierFactory",
        {delayPath});
}

struct NativeApis final {
    LdrLoadDllFunction load = nullptr;
    LdrGetProcedureAddressFunction resolve = nullptr;
    LdrGetProcedureAddressExFunction resolveEx = nullptr;
    LdrGetProcedureAddressForCallerFunction resolveForCaller = nullptr;
};

NativeApis ResolveNativeApis() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return {
        Resolve<LdrLoadDllFunction>(ntdll, "LdrLoadDll"),
        Resolve<LdrGetProcedureAddressFunction>(
            ntdll,
            "LdrGetProcedureAddress"),
        Resolve<LdrGetProcedureAddressExFunction>(
            ntdll,
            "LdrGetProcedureAddressEx"),
        Resolve<LdrGetProcedureAddressForCallerFunction>(
            ntdll,
            "LdrGetProcedureAddressForCaller"),
    };
}

UNICODE_STRING NativeName(const std::wstring& path) {
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(path.data());
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    return name;
}

ANSI_STRING NativeProcedure(char* const value) {
    ANSI_STRING name{};
    name.Buffer = value;
    name.Length = static_cast<USHORT>(std::strlen(value));
    name.MaximumLength = name.Length;
    return name;
}

int RunDirectNativeLoad(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    if (native.load == nullptr || native.resolve == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("direct native load setup failed");
    }
    auto moduleName = NativeName(fakePath);
    HANDLE module = nullptr;
    const auto loadStatus = native.load(
        nullptr,
        nullptr,
        &moduleName,
        &module);
    char factoryName[] = "FakeSteamFactory";
    auto procedureName = NativeProcedure(factoryName);
    void* factoryAddress = nullptr;
    const auto symbolStatus = module == nullptr
        ? static_cast<NTSTATUS>(-1)
        : native.resolve(module, &procedureName, 0, &factoryAddress);
    void* const denied = factoryAddress == nullptr
        ? nullptr
        : reinterpret_cast<Factory>(factoryAddress)("SteamMatchMaking009");
    const bool guarded = loadStatus >= 0 && module != nullptr
        && symbolStatus >= 0
        && factoryAddress == DSRRandomizer::Steam::SteamFactoryDetourAddress(0)
        && denied != nullptr && !InvokeSynthetic(denied);
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
    return guarded && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("direct native load or symbol returned an unguarded result");
}

int RunDirectNativeSymbol(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    if (native.resolve == nullptr || module == nullptr || rawFactory == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("direct native symbol setup failed");
    }
    char factoryName[] = "FakeSteamFactory";
    auto procedureName = NativeProcedure(factoryName);
    void* result = nullptr;
    const auto status = native.resolve(
        module,
        &procedureName,
        0,
        &result);
    const bool guarded = status >= 0 && result != nullptr
        && result != reinterpret_cast<void*>(rawFactory)
        && result == DSRRandomizer::Steam::SteamFactoryDetourAddress(0);
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return guarded && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("direct native symbol returned the raw protected export");
}

int RunDirectNativeSymbolAlternates(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    const auto rawFactory = Resolve<Factory>(module, "FakeSteamFactory");
    const auto install = module == nullptr || rawFactory == nullptr
        ? DeferredModuleGateInstallStatus::InvalidConfiguration
        : InstallTestGate(Configuration(fakePath, false));
    if (native.resolveEx == nullptr || native.resolveForCaller == nullptr
        || module == nullptr || rawFactory == nullptr
        || install != DeferredModuleGateInstallStatus::Success) {
        std::cerr << "alternate setup ex=" << (native.resolveEx != nullptr)
                  << " caller=" << (native.resolveForCaller != nullptr)
                  << " module=" << (module != nullptr)
                  << " raw=" << (rawFactory != nullptr)
                  << " install=" << static_cast<int>(install)
                  << " fatal="
                  << (lastFatal.load() == nullptr
                        ? "(null)"
                        : lastFatal.load())
                  << '\n';
        return Fail("alternate native symbol setup failed");
    }
    char factoryName[] = "FakeSteamFactory";
    auto procedureName = NativeProcedure(factoryName);
    void* extendedResult = nullptr;
    const auto extendedStatus = native.resolveEx(
        module,
        &procedureName,
        0,
        &extendedResult,
        0);
    void* callerResult = nullptr;
    const auto callerStatus = native.resolveForCaller(
        module,
        &procedureName,
        0,
        &callerResult,
        0,
        reinterpret_cast<void*>(&RunDirectNativeSymbolAlternates));
    void* const detour = DSRRandomizer::Steam::SteamFactoryDetourAddress(0);
    const bool guarded = extendedStatus >= 0 && callerStatus >= 0
        && extendedResult == detour && callerResult == detour
        && extendedResult != reinterpret_cast<void*>(rawFactory);
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return guarded && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("alternate native resolver returned the raw protected export");
}

int RunNativeDelegationOneShot(const std::wstring& fakePath) {
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (module == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("native delegation probe setup failed");
    }
    const auto probe = DSRRandomizer::Modules::Testing::
        ProbeNativeSymbolDelegationChain();
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool oneShot = probe.firstOriginalCalls == 1
        && probe.chainedOriginalCalls == 0
        && probe.status < 0 && !probe.resultPublished
        && code != nullptr
        && std::strcmp(code, "STEAM_NATIVE_SYMBOL_MALFORMED") == 0;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return oneShot && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("native resolver chain received more than one delegation");
}

void* AllocateCountedMetadataPage(SIZE_T& pageSize) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    pageSize = system.dwPageSize;
    return VirtualAlloc(
        nullptr,
        pageSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
}

bool RestoreCountedMetadataPage(void* const page, const SIZE_T pageSize) {
    const auto oldProtection = countedMetadataOldProtection.load(
        std::memory_order_acquire);
    DWORD ignored = 0;
    return page != nullptr && pageSize != 0 && oldProtection != 0
        && VirtualProtect(page, pageSize, oldProtection, &ignored);
}

void PrepareCountedMetadataFault(void* const page, const SIZE_T pageSize) {
    countedMetadataOldProtection.store(0, std::memory_order_release);
    countedMetadataPageSize.store(pageSize, std::memory_order_release);
    countedMetadataPage.store(page, std::memory_order_release);
    DSRRandomizer::Modules::Testing::SetCountedStringSnapshotHook(
        &InvalidateCountedMetadataAfterSnapshot);
}

void ClearCountedMetadataFault() {
    DSRRandomizer::Modules::Testing::SetCountedStringSnapshotHook(nullptr);
    countedMetadataPage.store(nullptr, std::memory_order_release);
    countedMetadataPageSize.store(0, std::memory_order_release);
}

int RunCountedUnicodeSnapshot(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    if (native.load == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("counted Unicode snapshot setup failed");
    }
    SIZE_T pageSize = 0;
    void* const page = AllocateCountedMetadataPage(pageSize);
    if (page == nullptr) {
        return Fail("counted Unicode metadata allocation failed");
    }
    auto* const name = static_cast<UNICODE_STRING*>(page);
    *name = NativeName(fakePath);
    PrepareCountedMetadataFault(page, pageSize);
    HANDLE module = nullptr;
    const auto status = native.load(nullptr, nullptr, name, &module);
    const bool restored = RestoreCountedMetadataPage(page, pageSize);
    ClearCountedMetadataFault();
    const bool safeSnapshot = restored && status >= 0 && module != nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
    VirtualFree(page, 0, MEM_RELEASE);
    return safeSnapshot && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("native Unicode metadata was reread after validation");
}

int RunCountedAnsiSnapshot(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (native.resolve == nullptr || module == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("counted ANSI snapshot setup failed");
    }
    SIZE_T pageSize = 0;
    void* const page = AllocateCountedMetadataPage(pageSize);
    if (page == nullptr) {
        return Fail("counted ANSI metadata allocation failed");
    }
    char factoryName[] = "FakeSteamFactory";
    auto* const name = static_cast<ANSI_STRING*>(page);
    *name = NativeProcedure(factoryName);
    PrepareCountedMetadataFault(page, pageSize);
    void* factoryAddress = nullptr;
    const auto status = native.resolve(module, name, 0, &factoryAddress);
    const bool restored = RestoreCountedMetadataPage(page, pageSize);
    ClearCountedMetadataFault();
    const bool safeSnapshot = restored && status >= 0
        && factoryAddress == DSRRandomizer::Steam::SteamFactoryDetourAddress(0);
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    VirtualFree(page, 0, MEM_RELEASE);
    FreeLibrary(module);
    return safeSnapshot && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("native ANSI metadata was reread after validation");
}

int RunMalformedNativeLoad(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    if (native.load == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("malformed native load setup failed");
    }
    auto malformed = NativeName(fakePath);
    ++malformed.Length;
    HANDLE module = reinterpret_cast<HANDLE>(1);
    const auto status = native.load(nullptr, nullptr, &malformed, &module);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = status < 0 && module == nullptr
        && code != nullptr
        && std::strcmp(code, "STEAM_NATIVE_LOAD_MALFORMED") == 0
        && GetModuleHandleW(FileName(fakePath).c_str()) == nullptr;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("malformed native load string did not fail closed");
}

int RunAmbiguousNativeLoad(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    if (native.load == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("ambiguous native load setup failed");
    }
    auto ambiguousPath = FileName(fakePath);
    auto ambiguous = NativeName(ambiguousPath);
    HANDLE module = reinterpret_cast<HANDLE>(1);
    const auto status = native.load(nullptr, nullptr, &ambiguous, &module);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = status < 0 && module == nullptr
        && code != nullptr
        && std::strcmp(code, "STEAM_NATIVE_LOAD_PATH_AMBIGUOUS") == 0;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("ambiguous native load path did not fail closed");
}

int RunMalformedNativeSymbol(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (native.resolve == nullptr || module == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("malformed native symbol setup failed");
    }
    char factoryName[] = "FakeSteamFactory";
    auto malformed = NativeProcedure(factoryName);
    malformed.MaximumLength = static_cast<USHORT>(malformed.Length - 1);
    void* result = reinterpret_cast<void*>(1);
    const auto status = native.resolve(module, &malformed, 0, &result);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = status < 0 && result == nullptr
        && code != nullptr
        && std::strcmp(code, "STEAM_NATIVE_SYMBOL_MALFORMED") == 0;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("malformed native symbol string did not fail closed");
}

int RunNativeOrdinal(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    const HMODULE module = LoadLibraryW(fakePath.c_str());
    if (native.resolve == nullptr || module == nullptr
        || InstallTestGate(Configuration(fakePath, false))
            != DeferredModuleGateInstallStatus::Success) {
        return Fail("native ordinal setup failed");
    }
    void* result = reinterpret_cast<void*>(1);
    const auto status = native.resolve(module, nullptr, 1, &result);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = status < 0 && result == nullptr
        && code != nullptr
        && std::strcmp(code, "STEAM_SYMBOL_UNSUPPORTED") == 0;
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    FreeLibrary(module);
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("native protected ordinal did not fail before raw resolution");
}

int RunNativeLoaderCallout(
    const std::wstring& fakePath,
    const std::wstring& calloutPath,
    const wchar_t* const mode,
    const char* const expectedFatal,
    const bool eager) {
    HMODULE fake = nullptr;
    if (eager) {
        fake = LoadLibraryW(fakePath.c_str());
    }
    if ((eager && fake == nullptr)
        || InstallTestGate(Configuration(fakePath, !eager))
            != DeferredModuleGateInstallStatus::Success
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_TARGET",
            fakePath.c_str())
        || !SetEnvironmentVariableW(
            L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_MODE",
            mode)) {
        return Fail("native loader-callout setup failed");
    }
    void* outer = nullptr;
    static_cast<void>(DSRRandomizer::Modules::Testing::
        CallOriginalLdrLoadDllForSyntheticCallout(calloutPath, &outer));
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_TARGET", nullptr);
    SetEnvironmentVariableW(L"DSR_RANDOMIZER_SYNTHETIC_NATIVE_MODE", nullptr);
    const auto code = lastFatal.load(std::memory_order_acquire);
    const bool denied = fatalCount.load(std::memory_order_acquire) != 0
        && code != nullptr && std::strcmp(code, expectedFatal) == 0
        && (eager || GetModuleHandleW(FileName(fakePath).c_str()) == nullptr);
    const auto cleanup = DSRRandomizer::Modules::UninstallDeferredModuleGate();
    if (outer != nullptr) {
        FreeLibrary(static_cast<HMODULE>(outer));
    }
    if (fake != nullptr) {
        FreeLibrary(fake);
    }
    return denied && cleanup == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("native loader callout published a protected result");
}

int RunNativeCleanup(const std::wstring& fakePath) {
    const auto native = ResolveNativeApis();
    if (native.load == nullptr || native.resolve == nullptr
        || InstallTestGate(Configuration(fakePath, true))
            != DeferredModuleGateInstallStatus::Success
        || DSRRandomizer::Modules::UninstallDeferredModuleGate()
            != DeferredModuleGateCleanupStatus::Success) {
        return Fail("native cleanup setup failed");
    }
    auto moduleName = NativeName(fakePath);
    HANDLE module = nullptr;
    const auto loadStatus = native.load(nullptr, nullptr, &moduleName, &module);
    char factoryName[] = "FakeSteamFactory";
    auto procedureName = NativeProcedure(factoryName);
    void* nativeFactory = nullptr;
    const auto symbolStatus = module == nullptr
        ? static_cast<NTSTATUS>(-1)
        : native.resolve(module, &procedureName, 0, &nativeFactory);
    const auto win32Factory = module == nullptr
        ? nullptr
        : GetProcAddress(
            static_cast<HMODULE>(module),
            "FakeSteamFactory");
    const bool restored = loadStatus >= 0 && symbolStatus >= 0
        && module != nullptr && nativeFactory != nullptr
        && nativeFactory == reinterpret_cast<void*>(win32Factory);
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
    return restored ? 0 : Fail("native hooks survived successful gate cleanup");
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
    if (DSRRandomizer::Modules::InstallDeferredModuleGate(
            Configuration(fakePath, true))
        != DeferredModuleGateInstallStatus::Success) {
        return Fail("production suspended-initializer gate was not armed");
    }
    return DSRRandomizer::Modules::UninstallDeferredModuleGate()
            == DeferredModuleGateCleanupStatus::Success
        ? 0
        : Fail("production suspended-initializer gate did not clean up");
}

DWORD RunChild(
    const std::wstring& mode,
    const std::wstring& fakePath,
    const std::wstring& nestedPath,
    const std::wstring& bridgePath,
    const std::wstring& carrierPath,
    const std::wstring& calloutPath) {
    auto command = Quote(CurrentExecutablePath()) + L" --child " + mode
        + L" " + Quote(fakePath) + L" " + Quote(nestedPath)
        + L" " + Quote(bridgePath) + L" " + Quote(carrierPath)
        + L" " + Quote(calloutPath);
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
    const std::wstring& carrierPath,
    const std::wstring& calloutPath) {
    constexpr std::array modes{
        L"returning-fatal",
        L"identity-teardown",
        L"mutation-barrier",
        L"post-create-failure",
        L"applied-factory-reporter",
        L"active-factory-rollback",
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
        L"protected-outer-direct",
        L"protected-outer-intermediate",
        L"protected-outer-delay",
        L"direct-native-load",
        L"direct-native-symbol",
        L"direct-native-symbol-alternates",
        L"native-delegation-one-shot",
        L"counted-unicode-snapshot",
        L"counted-ansi-snapshot",
        L"malformed-native-load",
        L"ambiguous-native-load",
        L"malformed-native-symbol",
        L"native-ordinal",
        L"native-load-callout",
        L"win32-symbol-callout",
        L"native-symbol-callout",
        L"native-cleanup",
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
            carrierPath,
            calloutPath);
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
    if (argc == 6) {
        return RunParent(
            CanonicalDosPath(argv[1]),
            CanonicalDosPath(argv[2]),
            CanonicalDosPath(argv[3]),
            CanonicalDosPath(argv[4]),
            CanonicalDosPath(argv[5]));
    }
    if (argc == 8 && std::wstring_view(argv[1]) == L"--child") {
        const std::wstring_view mode(argv[2]);
        const std::wstring fakePath = CanonicalDosPath(argv[3]);
        const std::wstring nestedPath = CanonicalDosPath(argv[4]);
        const std::wstring bridgePath = CanonicalDosPath(argv[5]);
        const std::wstring carrierPath = CanonicalDosPath(argv[6]);
        const std::wstring calloutPath = CanonicalDosPath(argv[7]);
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
        if (mode == L"applied-factory-reporter") {
            return RunAppliedFactoryReporterRetention(fakePath);
        }
        if (mode == L"active-factory-rollback") {
            return RunActiveFactoryRollback(fakePath);
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
        if (mode == L"protected-outer-direct") {
            return RunProtectedOuterDirect(fakePath, carrierPath);
        }
        if (mode == L"protected-outer-intermediate") {
            return RunProtectedOuterIntermediate(fakePath, carrierPath);
        }
        if (mode == L"protected-outer-delay") {
            return RunProtectedOuterDelay(fakePath, carrierPath);
        }
        if (mode == L"direct-native-load") {
            return RunDirectNativeLoad(fakePath);
        }
        if (mode == L"direct-native-symbol") {
            return RunDirectNativeSymbol(fakePath);
        }
        if (mode == L"direct-native-symbol-alternates") {
            return RunDirectNativeSymbolAlternates(fakePath);
        }
        if (mode == L"native-delegation-one-shot") {
            return RunNativeDelegationOneShot(fakePath);
        }
        if (mode == L"counted-unicode-snapshot") {
            return RunCountedUnicodeSnapshot(fakePath);
        }
        if (mode == L"counted-ansi-snapshot") {
            return RunCountedAnsiSnapshot(fakePath);
        }
        if (mode == L"malformed-native-load") {
            return RunMalformedNativeLoad(fakePath);
        }
        if (mode == L"ambiguous-native-load") {
            return RunAmbiguousNativeLoad(fakePath);
        }
        if (mode == L"malformed-native-symbol") {
            return RunMalformedNativeSymbol(fakePath);
        }
        if (mode == L"native-ordinal") {
            return RunNativeOrdinal(fakePath);
        }
        if (mode == L"native-load-callout") {
            return RunNativeLoaderCallout(
                fakePath,
                calloutPath,
                L"load",
                "STEAM_LOADER_CALLOUT_LOAD_DENIED",
                false);
        }
        if (mode == L"win32-symbol-callout") {
            return RunNativeLoaderCallout(
                fakePath,
                calloutPath,
                L"getproc",
                "STEAM_LOADER_CALLOUT_SYMBOL_DENIED",
                true);
        }
        if (mode == L"native-symbol-callout") {
            return RunNativeLoaderCallout(
                fakePath,
                calloutPath,
                L"ldrgetproc",
                "STEAM_LOADER_CALLOUT_SYMBOL_DENIED",
                true);
        }
        if (mode == L"native-cleanup") {
            return RunNativeCleanup(fakePath);
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

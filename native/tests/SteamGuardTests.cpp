#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <memory>
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
using DSRRandomizer::Steam::InterfaceDecision;
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

std::array<void*, 29> productionUserVTable{};
std::array<void*, 36> productionClientVTable{};
std::array<void*, 38> productionMatchmakingVTable{};
std::array<void*, 22> productionNetworkingVTable{};
std::array<void*, 55> productionRemoteStorageVTable{};
struct ProductionRawInterface {
    void** vtable;
};
ProductionRawInterface productionUser{productionUserVTable.data()};
ProductionRawInterface productionClient{productionClientVTable.data()};
ProductionRawInterface productionMatchmaking{productionMatchmakingVTable.data()};
ProductionRawInterface productionNetworking{productionNetworkingVTable.data()};
ProductionRawInterface productionRemoteStorage{productionRemoteStorageVTable.data()};
std::uint32_t productionRawProtectedCalls = 0;
std::uint32_t productionRawIdentityCalls = 0;
std::uint32_t productionRawClientGetterCalls = 0;
std::uint32_t productionAbiFatalCalls = 0;
constexpr std::uint64_t kProductionSteamId = 0x0110000101234567ULL;

#pragma pack(push, 1)
class ProductionSteamId final {
public:
    ProductionSteamId() noexcept : value_(0) {}
    explicit ProductionSteamId(const std::uint64_t value) noexcept
        : value_(value) {}

    [[nodiscard]] std::uint64_t Value() const noexcept { return value_; }

private:
    std::uint64_t value_;
};
#pragma pack(pop)

static_assert(sizeof(ProductionSteamId) == sizeof(std::uint64_t));

void* __cdecl ProductionRawFactory(const char* version) noexcept;

std::uintptr_t ProductionRawProtected(void*, ...) noexcept {
    ++productionRawProtectedCalls;
    return UINTPTR_MAX;
}

ProductionSteamId ProductionRawSteamId(void*) noexcept {
    ++productionRawIdentityCalls;
    return ProductionSteamId(kProductionSteamId);
}

void* ProductionRawClientGetter(
    void*,
    std::int32_t,
    std::int32_t,
    const char* const version) noexcept {
    ++productionRawClientGetterCalls;
    return ProductionRawFactory(version);
}

void* ProductionRawClientUtilsGetter(
    void*,
    std::int32_t,
    const char* const version) noexcept {
    ++productionRawClientGetterCalls;
    return ProductionRawFactory(version);
}

void* __cdecl ProductionRawFactory(const char* const version) noexcept {
    if (version == nullptr) {
        return nullptr;
    }
    if (std::strcmp(version, "SteamUser019") == 0) {
        return &productionUser;
    }
    if (std::strcmp(version, "SteamClient017") == 0) {
        return &productionClient;
    }
    if (std::strcmp(version, "SteamMatchMaking009") == 0) {
        return &productionMatchmaking;
    }
    if (std::strcmp(version, "SteamNetworking005") == 0) {
        return &productionNetworking;
    }
    if (std::strcmp(
            version,
            "STEAMREMOTESTORAGE_INTERFACE_VERSION014") == 0) {
        return &productionRemoteStorage;
    }
    return reinterpret_cast<void*>(1);
}

void ProductionAbiFatalReporter(const char*) noexcept {
    ++productionAbiFatalCalls;
}

template <typename Result, typename... Arguments>
Result CallProductionSlot(
    void* const interfaceValue,
    const std::size_t slot,
    Arguments... arguments) {
    auto** const vtable = *reinterpret_cast<void***>(interfaceValue);
    const auto method = reinterpret_cast<Result(*)(void*, Arguments...) noexcept>(
        vtable[slot]);
    return method(interfaceValue, arguments...);
}

bool CallEveryProductionSlot(
    void* const value,
    const std::size_t count,
    const std::initializer_list<std::size_t> classReturnSlots = {}) {
    if (value == nullptr) {
        return false;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (std::find(
                classReturnSlots.begin(),
                classReturnSlots.end(),
                slot) != classReturnSlots.end()) {
            continue;
        }
        static_cast<void>(CallProductionSlot<std::uintptr_t>(value, slot));
    }
    return true;
}

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
        || policy.Evaluate("SteamNetworking005", SteamMethod::SendP2PPacket)
            != MethodDecision::Deny
        || policy.Evaluate(
            "STEAMREMOTESTORAGE_INTERFACE_VERSION014",
            SteamMethod::FileWrite) != MethodDecision::Deny
        || policy.Evaluate("SteamUser019", SteamMethod::GetSteamID)
            != MethodDecision::Allow
        || policy.EvaluateInterface("SteamClient017")
            != InterfaceDecision::WrapClient
        || policy.Evaluate("SteamMatchMaking999", SteamMethod::CreateLobby)
            != MethodDecision::UnknownInterfaceFatal) {
        return Fail("Steam interface policy matrix was not fail closed");
    }
    return 0;
}

int VerifyProductionPinnedAbi() {
    productionUserVTable.fill(reinterpret_cast<void*>(&ProductionRawProtected));
    productionClientVTable.fill(reinterpret_cast<void*>(&ProductionRawProtected));
    productionMatchmakingVTable.fill(
        reinterpret_cast<void*>(&ProductionRawProtected));
    productionNetworkingVTable.fill(
        reinterpret_cast<void*>(&ProductionRawProtected));
    productionRemoteStorageVTable.fill(
        reinterpret_cast<void*>(&ProductionRawProtected));
    productionUserVTable[2] = reinterpret_cast<void*>(&ProductionRawSteamId);
    for (const auto slot : {5U, 6U, 8U, 10U, 11U, 12U, 13U, 14U, 15U,
                            16U, 17U, 18U, 23U, 24U, 25U, 26U, 27U, 28U,
                            29U, 30U, 34U, 35U}) {
        productionClientVTable[slot] =
            reinterpret_cast<void*>(&ProductionRawClientGetter);
    }
    productionClientVTable[9] =
        reinterpret_cast<void*>(&ProductionRawClientUtilsGetter);
    productionRawProtectedCalls = 0;
    productionRawIdentityCalls = 0;
    productionRawClientGetterCalls = 0;
    productionAbiFatalCalls = 0;

    constexpr std::size_t slot = 7;
    const std::vector<std::string> declared{
        "SteamClient017",
        "SteamUser019",
        "SteamMatchMaking009",
        "SteamNetworking005",
        "STEAMREMOTESTORAGE_INTERFACE_VERSION014",
    };
    const auto fatalState = std::make_shared<DSRRandomizer::Steam::FatalState>(
        &ProductionAbiFatalReporter);
    if (DSRRandomizer::Steam::RegisterSteamFactorySlot(
            slot,
            declared,
            fatalState,
            DSRRandomizer::Steam::SteamInterfaceLayout::ProductionPinned)
            != DSRRandomizer::Steam::SteamFactorySlotStatus::Success
        || !DSRRandomizer::Steam::SetSteamFactoryOriginal(
            slot,
            &ProductionRawFactory)) {
        return Fail("production ABI factory slot could not be registered");
    }
    const auto factory = reinterpret_cast<Factory>(
        DSRRandomizer::Steam::SteamFactoryDetourAddress(slot));
    void* const client = factory("SteamClient017");
    void* const user = CallProductionSlot<void*>(
        client, 5, std::int32_t{}, std::int32_t{}, "SteamUser019");
    void* const matchmaking = CallProductionSlot<void*>(
        client, 10, std::int32_t{}, std::int32_t{}, "SteamMatchMaking009");
    void* const networking = CallProductionSlot<void*>(
        client, 16, std::int32_t{}, std::int32_t{}, "SteamNetworking005");
    void* const remoteStorage = CallProductionSlot<void*>(
        client, 17, std::int32_t{}, std::int32_t{},
        "STEAMREMOTESTORAGE_INTERFACE_VERSION014");
    const bool noRawProtectedObject = client != &productionClient
        && user != &productionUser
        && matchmaking != &productionMatchmaking
        && networking != &productionNetworking
        && remoteStorage != &productionRemoteStorage;
    const bool exactIdentity = CallProductionSlot<ProductionSteamId>(user, 2)
            .Value() == kProductionSteamId
        && productionRawIdentityCalls == 1;
    const bool representativeSlots =
        CallProductionSlot<std::int32_t>(user, 0) == 0
        && CallProductionSlot<std::int32_t>(
            user, 14, static_cast<const void*>(nullptr), 0,
            std::uint64_t{}) == 0
        && !CallProductionSlot<bool>(user, 28)
        && CallProductionSlot<std::int32_t>(matchmaking, 0) == 0
        && std::strcmp(
            CallProductionSlot<const char*>(
                matchmaking, 19, std::uint64_t{}, "key"),
            "") == 0
        && CallProductionSlot<std::uint64_t>(
            matchmaking, 13, std::int32_t{}, 4) == 0
        && CallProductionSlot<ProductionSteamId>(
            matchmaking, 12, std::int32_t{}).Value() == 0
        && CallProductionSlot<ProductionSteamId>(
            matchmaking, 18, std::uint64_t{}, std::int32_t{}).Value() == 0
        && CallProductionSlot<ProductionSteamId>(
            matchmaking, 35, std::uint64_t{}).Value() == 0
        && !CallProductionSlot<bool>(
            matchmaking, 37, std::uint64_t{}, std::uint64_t{})
        && !CallProductionSlot<bool>(
            networking, 0, std::uint64_t{}, nullptr, std::uint32_t{},
            std::int32_t{}, 0)
        && !CallProductionSlot<bool>(
            networking, 11, std::uint32_t{}, false)
        && CallProductionSlot<std::int32_t>(
            networking, 21, std::uint32_t{}) == 0
        && !CallProductionSlot<bool>(
            remoteStorage, 0, "save", nullptr, std::int32_t{})
        && CallProductionSlot<std::uint64_t>(
            remoteStorage, 29, std::int32_t{}) == UINT64_MAX
        && CallProductionSlot<std::uint64_t>(
            remoteStorage, 54, std::uint64_t{}, "path", std::uint32_t{}) == 0;
    const bool everySlotCallable = CallEveryProductionSlot(user, 29, {2})
        && CallEveryProductionSlot(matchmaking, 38, {12, 18, 35})
        && CallEveryProductionSlot(networking, 22)
        && CallEveryProductionSlot(remoteStorage, 55);
    const bool exactWindowsSlotCounts =
        DSRRandomizer::Steam::Testing::ProductionInterfaceSlotCount(
            "SteamClient017") == 36
        && DSRRandomizer::Steam::Testing::ProductionInterfaceSlotCount(
            "SteamUser019") == 29
        && DSRRandomizer::Steam::Testing::ProductionInterfaceSlotCount(
            "SteamMatchMaking009") == 38
        && DSRRandomizer::Steam::Testing::ProductionInterfaceSlotCount(
            "SteamNetworking005") == 22
        && DSRRandomizer::Steam::Testing::ProductionInterfaceSlotCount(
            "STEAMREMOTESTORAGE_INTERFACE_VERSION014") == 55;
    const bool protectedRawNeverCalled = productionRawProtectedCalls == 0
        && productionRawClientGetterCalls >= 4;

    const auto clientGetterCallsBeforeUnknown = productionRawClientGetterCalls;
    const bool unknownClientGatewayDenied = CallProductionSlot<void*>(
            client,
            12,
            std::int32_t{},
            std::int32_t{},
            "SteamNetworking999") == nullptr
        && productionRawClientGetterCalls == clientGetterCallsBeforeUnknown
        && fatalState->IsFatal();

    fatalState->EnterDenyOnly();
    DSRRandomizer::Steam::UnregisterSteamFactorySlot(slot);
    const bool teardownDeny = CallProductionSlot<ProductionSteamId>(user, 2)
            .Value() == 0
        && !CallProductionSlot<bool>(
            networking, 0, std::uint64_t{}, nullptr, std::uint32_t{},
            std::int32_t{}, 0)
        && factory("SteamUser019") == nullptr
        && productionRawIdentityCalls == 1;

    productionAbiFatalCalls = 0;
    const auto returningFatal = std::make_shared<DSRRandomizer::Steam::FatalState>(
        &ProductionAbiFatalReporter);
    const bool returningFatalRegistered =
        DSRRandomizer::Steam::RegisterSteamFactorySlot(
            slot,
            declared,
            returningFatal,
            DSRRandomizer::Steam::SteamInterfaceLayout::ProductionPinned)
            == DSRRandomizer::Steam::SteamFactorySlotStatus::Success
        && DSRRandomizer::Steam::SetSteamFactoryOriginal(
            slot,
            &ProductionRawFactory);
    const bool returningFatalDenied = returningFatalRegistered
        && factory("SteamMatchMaking999") == nullptr
        && productionAbiFatalCalls == 1
        && factory("SteamUser019") == nullptr;
    DSRRandomizer::Steam::UnregisterSteamFactorySlot(slot);

    return noRawProtectedObject && exactIdentity && representativeSlots
            && everySlotCallable && exactWindowsSlotCounts
            && protectedRawNeverCalled && unknownClientGatewayDenied
            && teardownDeny
            && returningFatalDenied
        ? 0
        : Fail("production version-pinned Steam ABI was not offline-safe");
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
    if (VerifyProductionPinnedAbi() != 0) {
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

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
#include <type_traits>
#include <utility>
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

#pragma pack(push, 1)
class ProductionGameId final {
public:
    ProductionGameId() noexcept : value_(0) {}
    explicit ProductionGameId(const std::uint64_t value) noexcept
        : value_(value) {}

private:
    std::uint64_t value_;
};
#pragma pack(pop)

struct ProductionP2PSessionState {
    std::uint8_t connectionActive;
    std::uint8_t connecting;
    std::uint8_t sessionError;
    std::uint8_t usingRelay;
    std::int32_t bytesQueuedForSend;
    std::int32_t packetsQueuedForSend;
    std::uint32_t remoteIp;
    std::uint16_t remotePort;
};

static_assert(sizeof(ProductionGameId) == sizeof(std::uint64_t));
static_assert(sizeof(ProductionP2PSessionState) == 20);

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

using I32 = std::int32_t;
using I64 = std::int64_t;
using U16 = std::uint16_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using ProductionWarningMessageHook = void(__cdecl*)(I32, const char*);
using ProductionPostApiResultHook = void(__cdecl*)();
using ProductionCheckCallbackRegisteredHook = U32(__cdecl*)(I32);

struct ProductionSteamParamStringArray;

// These declarations are the reviewed Windows ABI matrix.  Each invocation
// below is compiled through its slot's exact method type instead of deriving a
// heterogeneous function pointer from the supplied test arguments.
struct SteamClient017Abi;
struct SteamUser019Abi;
struct SteamMatchmaking009Abi;
struct SteamNetworking005Abi;
struct SteamRemoteStorage014Abi;

template <typename Result, typename... Arguments>
using ProductionMethod = Result(*)(void*, Arguments...) noexcept;

template <typename Interface, std::size_t Slot>
struct ProductionSlotSignature;

#define DECLARE_SLOT_SIGNATURE0(Interface, Slot, Result)       \
    template <>                                               \
    struct ProductionSlotSignature<Interface, Slot> final {   \
        using Method = ProductionMethod<Result>;              \
    }

#define DECLARE_SLOT_SIGNATURE(Interface, Slot, Result, ...)   \
    template <>                                               \
    struct ProductionSlotSignature<Interface, Slot> final {   \
        using Method = ProductionMethod<Result, __VA_ARGS__>; \
    }

DECLARE_SLOT_SIGNATURE0(SteamClient017Abi, 0, I32);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 1, bool, I32);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 2, I32, I32);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 3, I32, I32*, I32);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 4, void, I32, I32);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 5, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 6, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 7, void, U32, U16);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 8, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 9, void*, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 10, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 11, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 12, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 13, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 14, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 15, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 16, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 17, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 18, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE0(SteamClient017Abi, 19, void);
DECLARE_SLOT_SIGNATURE0(SteamClient017Abi, 20, U32);
DECLARE_SLOT_SIGNATURE(
    SteamClient017Abi, 21, void, ProductionWarningMessageHook);
DECLARE_SLOT_SIGNATURE0(SteamClient017Abi, 22, bool);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 23, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 24, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 25, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 26, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 27, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 28, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 29, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 30, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(
    SteamClient017Abi, 31, void, ProductionPostApiResultHook);
DECLARE_SLOT_SIGNATURE(
    SteamClient017Abi, 32, void, ProductionPostApiResultHook);
DECLARE_SLOT_SIGNATURE(
    SteamClient017Abi, 33, void, ProductionCheckCallbackRegisteredHook);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 34, void*, I32, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamClient017Abi, 35, void*, I32, I32, const char*);

DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 0, I32);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 1, bool);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 2, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamUser019Abi,
    3,
    I32,
    void*,
    I32,
    ProductionSteamId,
    U32,
    U16,
    bool);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 4, void, U32, U16);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 5, void, ProductionGameId, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 6, bool, char*, I32);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 7, void);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 8, void);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 9, I32, U32*, U32*, U32);
DECLARE_SLOT_SIGNATURE(
    SteamUser019Abi,
    10,
    I32,
    bool,
    void*,
    U32,
    U32*,
    bool,
    void*,
    U32,
    U32*,
    U32);
DECLARE_SLOT_SIGNATURE(
    SteamUser019Abi, 11, I32, const void*, U32, void*, U32, U32*, U32);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 12, U32);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 13, U32, void*, I32, U32*);
DECLARE_SLOT_SIGNATURE(
    SteamUser019Abi, 14, I32, const void*, I32, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 15, void, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 16, void, U32);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 17, I32, ProductionSteamId, U32);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 18, bool);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 19, void, ProductionSteamId, U32, U16);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 20, U64, const void*, I32);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 21, bool, void*, I32, U32*);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 22, I32, I32, bool);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 23, I32);
DECLARE_SLOT_SIGNATURE(SteamUser019Abi, 24, U64, const char*);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 25, bool);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 26, bool);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 27, bool);
DECLARE_SLOT_SIGNATURE0(SteamUser019Abi, 28, bool);

DECLARE_SLOT_SIGNATURE0(SteamMatchmaking009Abi, 0, I32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 1, bool, I32, U32*, U32*, U16*, U16*, U32*, U32*);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 2, I32, U32, U32, U16, U16, U32, U32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 3, bool, U32, U32, U16, U16, U32);
DECLARE_SLOT_SIGNATURE0(SteamMatchmaking009Abi, 4, U64);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 5, void, const char*, const char*, I32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 6, void, const char*, I32, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 7, void, const char*, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 8, void, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 9, void, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 10, void, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 11, void, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 12, ProductionSteamId, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 13, U64, I32, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 14, U64, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 15, void, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 16, bool, ProductionSteamId, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 17, I32, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 18, ProductionSteamId, ProductionSteamId, I32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 19, const char*, ProductionSteamId, const char*);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 20, bool, ProductionSteamId, const char*, const char*);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 21, I32, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi,
    22,
    bool,
    ProductionSteamId,
    I32,
    char*,
    I32,
    char*,
    I32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 23, bool, ProductionSteamId, const char*);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi,
    24,
    const char*,
    ProductionSteamId,
    ProductionSteamId,
    const char*);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 25, void, ProductionSteamId, const char*, const char*);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 26, bool, ProductionSteamId, const void*, I32);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi,
    27,
    I32,
    ProductionSteamId,
    I32,
    ProductionSteamId*,
    void*,
    I32,
    I32*);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 28, bool, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi,
    29,
    void,
    ProductionSteamId,
    U32,
    U16,
    ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi,
    30,
    bool,
    ProductionSteamId,
    U32*,
    U16*,
    ProductionSteamId*);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 31, bool, ProductionSteamId, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 32, I32, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 33, bool, ProductionSteamId, I32);
DECLARE_SLOT_SIGNATURE(SteamMatchmaking009Abi, 34, bool, ProductionSteamId, bool);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 35, ProductionSteamId, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 36, bool, ProductionSteamId, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(
    SteamMatchmaking009Abi, 37, bool, ProductionSteamId, ProductionSteamId);

DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi, 0, bool, ProductionSteamId, const void*, U32, I32, I32);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 1, bool, U32*, I32);
DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi, 2, bool, void*, U32, U32*, ProductionSteamId*, I32);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 3, bool, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 4, bool, ProductionSteamId);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 5, bool, ProductionSteamId, I32);
DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi, 6, bool, ProductionSteamId, ProductionP2PSessionState*);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 7, bool, bool);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 8, U32, I32, U32, U16, bool);
DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi, 9, U32, ProductionSteamId, I32, I32, bool);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 10, U32, U32, U16, I32);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 11, bool, U32, bool);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 12, bool, U32, bool);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 13, bool, U32, void*, U32, bool);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 14, bool, U32, U32*);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 15, bool, U32, void*, U32, U32*);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 16, bool, U32, U32*, U32*);
DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi, 17, bool, U32, void*, U32, U32*, U32*);
DECLARE_SLOT_SIGNATURE(
    SteamNetworking005Abi,
    18,
    bool,
    U32,
    ProductionSteamId*,
    I32*,
    U32*,
    U16*);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 19, bool, U32, U32*, U16*);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 20, I32, U32);
DECLARE_SLOT_SIGNATURE(SteamNetworking005Abi, 21, I32, U32);

DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 0, bool, const char*, const void*, I32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 1, I32, const char*, void*, I32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 2, U64, const char*, const void*, U32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 3, U64, const char*, U32, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 4, bool, U64, void*, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 5, bool, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 6, bool, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 7, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 8, bool, const char*, I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 9, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 10, bool, U64, const void*, I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 11, bool, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 12, bool, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 13, bool, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 14, bool, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 15, I32, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 16, I64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 17, I32, const char*);
DECLARE_SLOT_SIGNATURE0(SteamRemoteStorage014Abi, 18, I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 19, const char*, I32, I32*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 20, bool, U64*, U64*);
DECLARE_SLOT_SIGNATURE0(SteamRemoteStorage014Abi, 21, bool);
DECLARE_SLOT_SIGNATURE0(SteamRemoteStorage014Abi, 22, bool);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 23, void, bool);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 24, U64, U64, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 25, bool, U64, I32*, I32*);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 26, bool, U64, U32*, char**, I32*, ProductionSteamId*);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 27, I32, U64, void*, I32, U32, I32);
DECLARE_SLOT_SIGNATURE0(SteamRemoteStorage014Abi, 28, I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 29, U64, I32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi,
    30,
    U64,
    const char*,
    const char*,
    U32,
    const char*,
    const char*,
    I32,
    ProductionSteamParamStringArray*,
    I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 31, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 32, bool, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 33, bool, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 34, bool, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 35, bool, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 36, bool, U64, I32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 37, bool, U64, ProductionSteamParamStringArray*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 38, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 39, U64, U64, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 40, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 41, U64, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 42, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 43, U64, U32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 44, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 45, bool, U64, const char*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 46, U64, U64);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 47, U64, U64, bool);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 48, U64, U64);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi,
    49,
    U64,
    ProductionSteamId,
    U32,
    ProductionSteamParamStringArray*,
    ProductionSteamParamStringArray*);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi,
    50,
    U64,
    I32,
    const char*,
    const char*,
    const char*,
    U32,
    const char*,
    const char*,
    I32,
    ProductionSteamParamStringArray*);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 51, U64, U64, I32);
DECLARE_SLOT_SIGNATURE(SteamRemoteStorage014Abi, 52, U64, I32, U32);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi,
    53,
    U64,
    I32,
    U32,
    U32,
    U32,
    ProductionSteamParamStringArray*,
    ProductionSteamParamStringArray*);
DECLARE_SLOT_SIGNATURE(
    SteamRemoteStorage014Abi, 54, U64, U64, const char*, U32);

#undef DECLARE_SLOT_SIGNATURE
#undef DECLARE_SLOT_SIGNATURE0

template <typename Interface, std::size_t Slot, typename... Arguments>
decltype(auto) CallProductionSlot(
    void* const interfaceValue,
    Arguments&&... arguments) {
    using Method = typename ProductionSlotSignature<Interface, Slot>::Method;
    static_assert(std::is_invocable_v<Method, void*, Arguments...>);
    auto** const vtable = *reinterpret_cast<void***>(interfaceValue);
    Method method = nullptr;
    static_assert(sizeof(method) == sizeof(vtable[Slot]));
    std::memcpy(&method, &vtable[Slot], sizeof(method));
    return method(interfaceValue, std::forward<Arguments>(arguments)...);
}

struct SlotMatrix final {
    bool valid = true;
    std::size_t calls = 0;

    template <typename Actual, typename Expected>
    void Equal(const Actual& actual, const Expected& expected) noexcept {
        ++calls;
        valid = (actual == expected) && valid;
    }

    template <typename Actual, typename Expected>
    void Output(const Actual& actual, const Expected& expected) noexcept {
        valid = (actual == expected) && valid;
    }

    void VoidCall() noexcept { ++calls; }

    template <typename Value, std::size_t Size>
    void Zeroed(const std::array<Value, Size>& values) noexcept {
        valid = std::all_of(
                    values.begin(),
                    values.end(),
                    [](const Value& value) { return value == Value{}; })
            && valid;
    }
};

bool VerifyUser019Slots(void* const user) {
    SlotMatrix matrix;
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 0>(user), 0);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 1>(user), false);
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 2>(user).Value(),
        kProductionSteamId);
    std::array<std::byte, 8> authBlob{};
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 3>(user,
            authBlob.data(),
            static_cast<I32>(authBlob.size()),
            ProductionSteamId{},
            U32{},
            U16{},
            false),
        0);
    CallProductionSlot<SteamUser019Abi, 4>(user, U32{}, U16{});
    matrix.VoidCall();
    CallProductionSlot<SteamUser019Abi, 5>(user, ProductionGameId{}, I32{}, "event");
    matrix.VoidCall();
    std::array<char, 8> userFolder;
    userFolder.fill('x');
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 6>(user, userFolder.data(), static_cast<I32>(userFolder.size())),
        false);
    matrix.Output(userFolder[0], '\0');
    CallProductionSlot<SteamUser019Abi, 7>(user);
    matrix.VoidCall();
    CallProductionSlot<SteamUser019Abi, 8>(user);
    matrix.VoidCall();
    U32 compressed = UINT32_MAX;
    U32 uncompressed = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 9>(user, &compressed, &uncompressed, U32{}),
        1);
    matrix.Output(compressed, U32{});
    matrix.Output(uncompressed, U32{});
    std::array<std::byte, 8> voiceCompressed;
    std::array<std::byte, 8> voiceUncompressed;
    voiceCompressed.fill(std::byte{0xa5});
    voiceUncompressed.fill(std::byte{0xa5});
    compressed = UINT32_MAX;
    uncompressed = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 10>(user,
            true,
            voiceCompressed.data(),
            static_cast<U32>(voiceCompressed.size()),
            &compressed,
            true,
            voiceUncompressed.data(),
            static_cast<U32>(voiceUncompressed.size()),
            &uncompressed,
            U32{}),
        1);
    matrix.Output(compressed, U32{});
    matrix.Output(uncompressed, U32{});
    matrix.Zeroed(voiceCompressed);
    matrix.Zeroed(voiceUncompressed);
    std::array<std::byte, 8> decompressed;
    decompressed.fill(std::byte{0xa5});
    U32 decompressedBytes = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 11>(user,
            voiceCompressed.data(),
            U32{},
            decompressed.data(),
            static_cast<U32>(decompressed.size()),
            &decompressedBytes,
            U32{}),
        1);
    matrix.Output(decompressedBytes, U32{});
    matrix.Zeroed(decompressed);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 12>(user), U32{});
    std::array<std::byte, 8> ticket;
    ticket.fill(std::byte{0xa5});
    U32 ticketBytes = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 13>(user,
            ticket.data(),
            static_cast<I32>(ticket.size()),
            &ticketBytes),
        U32{});
    matrix.Output(ticketBytes, U32{});
    matrix.Zeroed(ticket);
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 14>(user, ticket.data(), I32{}, ProductionSteamId{}),
        1);
    CallProductionSlot<SteamUser019Abi, 15>(user, ProductionSteamId{});
    matrix.VoidCall();
    CallProductionSlot<SteamUser019Abi, 16>(user, U32{});
    matrix.VoidCall();
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 17>(user, ProductionSteamId{}, U32{}),
        1);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 18>(user), false);
    CallProductionSlot<SteamUser019Abi, 19>(user, ProductionSteamId{}, U32{}, U16{});
    matrix.VoidCall();
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 20>(user, nullptr, I32{}), U64{});
    std::array<std::byte, 8> encryptedTicket;
    encryptedTicket.fill(std::byte{0xa5});
    U32 encryptedBytes = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamUser019Abi, 21>(user,
            encryptedTicket.data(),
            static_cast<I32>(encryptedTicket.size()),
            &encryptedBytes),
        false);
    matrix.Output(encryptedBytes, U32{});
    matrix.Zeroed(encryptedTicket);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 22>(user, I32{}, false), 0);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 23>(user), 0);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 24>(user, "redirect"), U64{});
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 25>(user), false);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 26>(user), false);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 27>(user), false);
    matrix.Equal(CallProductionSlot<SteamUser019Abi, 28>(user), false);
    return matrix.valid && matrix.calls == 29;
}

bool VerifyMatchmaking009Slots(void* const matchmaking) {
    SlotMatrix matrix;
    matrix.Equal(CallProductionSlot<SteamMatchmaking009Abi, 0>(matchmaking), 0);
    U32 appId = UINT32_MAX;
    U32 ip = UINT32_MAX;
    U16 connectionPort = UINT16_MAX;
    U16 queryPort = UINT16_MAX;
    U32 flags = UINT32_MAX;
    U32 lastPlayed = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 1>(matchmaking,
            I32{},
            &appId,
            &ip,
            &connectionPort,
            &queryPort,
            &flags,
            &lastPlayed),
        false);
    matrix.Output(appId, U32{});
    matrix.Output(ip, U32{});
    matrix.Output(connectionPort, U16{});
    matrix.Output(queryPort, U16{});
    matrix.Output(flags, U32{});
    matrix.Output(lastPlayed, U32{});
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 2>(matchmaking, U32{}, U32{}, U16{}, U16{}, U32{}, U32{}),
        0);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 3>(matchmaking, U32{}, U32{}, U16{}, U16{}, U32{}),
        false);
    matrix.Equal(CallProductionSlot<SteamMatchmaking009Abi, 4>(matchmaking), U64{});
    CallProductionSlot<SteamMatchmaking009Abi, 5>(matchmaking, "key", "value", I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 6>(matchmaking, "key", I32{}, I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 7>(matchmaking, "key", I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 8>(matchmaking, I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 9>(matchmaking, I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 10>(matchmaking, I32{});
    matrix.VoidCall();
    CallProductionSlot<SteamMatchmaking009Abi, 11>(matchmaking, ProductionSteamId{});
    matrix.VoidCall();
    matrix.Output(
        CallProductionSlot<SteamMatchmaking009Abi, 12>(matchmaking, I32{}).Value(),
        U64{});
    ++matrix.calls;
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 13>(matchmaking, I32{}, I32{}), U64{});
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 14>(matchmaking, ProductionSteamId{}), U64{});
    CallProductionSlot<SteamMatchmaking009Abi, 15>(matchmaking, ProductionSteamId{});
    matrix.VoidCall();
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 16>(matchmaking, ProductionSteamId{}, ProductionSteamId{}),
        false);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 17>(matchmaking, ProductionSteamId{}), 0);
    matrix.Output(
        CallProductionSlot<SteamMatchmaking009Abi, 18>(matchmaking, ProductionSteamId{}, I32{}).Value(),
        U64{});
    ++matrix.calls;
    matrix.Equal(
        std::strcmp(
            CallProductionSlot<SteamMatchmaking009Abi, 19>(matchmaking, ProductionSteamId{}, "key"),
            ""),
        0);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 20>(matchmaking, ProductionSteamId{}, "key", "value"),
        false);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 21>(matchmaking, ProductionSteamId{}), 0);
    std::array<char, 8> key;
    std::array<char, 8> value;
    key.fill('x');
    value.fill('x');
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 22>(matchmaking,
            ProductionSteamId{},
            I32{},
            key.data(),
            static_cast<I32>(key.size()),
            value.data(),
            static_cast<I32>(value.size())),
        false);
    matrix.Output(key[0], '\0');
    matrix.Output(value[0], '\0');
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 23>(matchmaking, ProductionSteamId{}, "key"),
        false);
    matrix.Equal(
        std::strcmp(
            CallProductionSlot<SteamMatchmaking009Abi, 24>(matchmaking,
                ProductionSteamId{},
                ProductionSteamId{},
                "key"),
            ""),
        0);
    CallProductionSlot<SteamMatchmaking009Abi, 25>(matchmaking, ProductionSteamId{}, "key", "value");
    matrix.VoidCall();
    std::array<std::byte, 8> chat;
    chat.fill(std::byte{0xa5});
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 26>(matchmaking,
            ProductionSteamId{},
            chat.data(),
            static_cast<I32>(chat.size())),
        false);
    ProductionSteamId chatUser(kProductionSteamId);
    I32 chatType = -1;
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 27>(matchmaking,
            ProductionSteamId{},
            I32{},
            &chatUser,
            chat.data(),
            static_cast<I32>(chat.size()),
            &chatType),
        0);
    matrix.Output(chatUser.Value(), U64{});
    matrix.Output(chatType, 0);
    matrix.Zeroed(chat);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 28>(matchmaking, ProductionSteamId{}), false);
    CallProductionSlot<SteamMatchmaking009Abi, 29>(matchmaking,
        ProductionSteamId{},
        U32{},
        U16{},
        ProductionSteamId{});
    matrix.VoidCall();
    ip = UINT32_MAX;
    connectionPort = UINT16_MAX;
    ProductionSteamId gameServer(kProductionSteamId);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 30>(matchmaking,
            ProductionSteamId{},
            &ip,
            &connectionPort,
            &gameServer),
        false);
    matrix.Output(ip, U32{});
    matrix.Output(connectionPort, U16{});
    matrix.Output(gameServer.Value(), U64{});
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 31>(matchmaking, ProductionSteamId{}, I32{}),
        false);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 32>(matchmaking, ProductionSteamId{}), 0);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 33>(matchmaking, ProductionSteamId{}, I32{}),
        false);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 34>(matchmaking, ProductionSteamId{}, false),
        false);
    matrix.Output(
        CallProductionSlot<SteamMatchmaking009Abi, 35>(matchmaking, ProductionSteamId{}).Value(),
        U64{});
    ++matrix.calls;
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 36>(matchmaking, ProductionSteamId{}, ProductionSteamId{}),
        false);
    matrix.Equal(
        CallProductionSlot<SteamMatchmaking009Abi, 37>(matchmaking, ProductionSteamId{}, ProductionSteamId{}),
        false);
    return matrix.valid && matrix.calls == 38;
}

bool VerifyNetworking005Slots(void* const networking) {
    SlotMatrix matrix;
    std::array<std::byte, 8> packet;
    packet.fill(std::byte{0xa5});
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 0>(networking,
            ProductionSteamId{},
            packet.data(),
            static_cast<U32>(packet.size()),
            I32{},
            I32{}),
        false);
    U32 messageSize = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 1>(networking, &messageSize, I32{}), false);
    matrix.Output(messageSize, U32{});
    messageSize = UINT32_MAX;
    ProductionSteamId remote(kProductionSteamId);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 2>(networking,
            packet.data(),
            static_cast<U32>(packet.size()),
            &messageSize,
            &remote,
            I32{}),
        false);
    matrix.Output(messageSize, U32{});
    matrix.Output(remote.Value(), U64{});
    matrix.Zeroed(packet);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 3>(networking, ProductionSteamId{}), false);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 4>(networking, ProductionSteamId{}), false);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 5>(networking, ProductionSteamId{}, I32{}),
        false);
    ProductionP2PSessionState state{};
    const ProductionP2PSessionState zeroState{};
    std::memset(&state, 0xa5, sizeof(state));
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 6>(networking, ProductionSteamId{}, &state),
        false);
    matrix.Output(
        std::memcmp(
            &state,
            &zeroState,
            sizeof(state)),
        0);
    matrix.Equal(CallProductionSlot<SteamNetworking005Abi, 7>(networking, false), false);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 8>(networking, I32{}, U32{}, U16{}, false),
        U32{});
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 9>(networking, ProductionSteamId{}, I32{}, I32{}, false),
        U32{});
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 10>(networking, U32{}, U16{}, I32{}),
        U32{});
    matrix.Equal(CallProductionSlot<SteamNetworking005Abi, 11>(networking, U32{}, false), false);
    matrix.Equal(CallProductionSlot<SteamNetworking005Abi, 12>(networking, U32{}, false), false);
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 13>(networking,
            U32{},
            packet.data(),
            static_cast<U32>(packet.size()),
            false),
        false);
    messageSize = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 14>(networking, U32{}, &messageSize), false);
    matrix.Output(messageSize, U32{});
    packet.fill(std::byte{0xa5});
    messageSize = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 15>(networking,
            U32{},
            packet.data(),
            static_cast<U32>(packet.size()),
            &messageSize),
        false);
    matrix.Output(messageSize, U32{});
    matrix.Zeroed(packet);
    messageSize = UINT32_MAX;
    U32 socket = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 16>(networking, U32{}, &messageSize, &socket),
        false);
    matrix.Output(messageSize, U32{});
    matrix.Output(socket, U32{});
    packet.fill(std::byte{0xa5});
    messageSize = UINT32_MAX;
    socket = UINT32_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 17>(networking,
            U32{},
            packet.data(),
            static_cast<U32>(packet.size()),
            &messageSize,
            &socket),
        false);
    matrix.Output(messageSize, U32{});
    matrix.Output(socket, U32{});
    matrix.Zeroed(packet);
    remote = ProductionSteamId(kProductionSteamId);
    I32 socketStatus = -1;
    U32 remoteIp = UINT32_MAX;
    U16 remotePort = UINT16_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 18>(networking,
            U32{},
            &remote,
            &socketStatus,
            &remoteIp,
            &remotePort),
        false);
    matrix.Output(remote.Value(), U64{});
    matrix.Output(socketStatus, 0);
    matrix.Output(remoteIp, U32{});
    matrix.Output(remotePort, U16{});
    remoteIp = UINT32_MAX;
    remotePort = UINT16_MAX;
    matrix.Equal(
        CallProductionSlot<SteamNetworking005Abi, 19>(networking, U32{}, &remoteIp, &remotePort),
        false);
    matrix.Output(remoteIp, U32{});
    matrix.Output(remotePort, U16{});
    matrix.Equal(CallProductionSlot<SteamNetworking005Abi, 20>(networking, U32{}), 0);
    matrix.Equal(CallProductionSlot<SteamNetworking005Abi, 21>(networking, U32{}), 0);
    return matrix.valid && matrix.calls == 22;
}

bool VerifyRemoteStorage014Slots(void* const storage) {
    SlotMatrix matrix;
    std::array<std::byte, 8> data;
    data.fill(std::byte{0xa5});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 0>(storage, "file", data.data(), static_cast<I32>(data.size())),
        false);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 1>(storage, "file", data.data(), static_cast<I32>(data.size())),
        0);
    matrix.Zeroed(data);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 2>(storage, "file", data.data(), static_cast<U32>(data.size())),
        U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 3>(storage, "file", U32{}, U32{}), U64{});
    data.fill(std::byte{0xa5});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 4>(storage, U64{}, data.data(), static_cast<U32>(data.size())),
        false);
    matrix.Zeroed(data);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 5>(storage, "file"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 6>(storage, "file"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 7>(storage, "file"), U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 8>(storage, "file", I32{}), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 9>(storage, "file"), UINT64_MAX);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 10>(storage, U64{}, data.data(), static_cast<I32>(data.size())),
        false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 11>(storage, U64{}), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 12>(storage, U64{}), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 13>(storage, "file"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 14>(storage, "file"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 15>(storage, "file"), 0);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 16>(storage, "file"), I64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 17>(storage, "file"), 0);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 18>(storage), 0);
    I32 fileSize = -1;
    matrix.Equal(
        std::strcmp(
            CallProductionSlot<SteamRemoteStorage014Abi, 19>(storage, I32{}, &fileSize),
            ""),
        0);
    matrix.Output(fileSize, 0);
    U64 totalBytes = UINT64_MAX;
    U64 availableBytes = UINT64_MAX;
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 20>(storage, &totalBytes, &availableBytes),
        false);
    matrix.Output(totalBytes, U64{});
    matrix.Output(availableBytes, U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 21>(storage), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 22>(storage), false);
    CallProductionSlot<SteamRemoteStorage014Abi, 23>(storage, false);
    matrix.VoidCall();
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 24>(storage, U64{}, U32{}), U64{});
    I32 downloaded = -1;
    I32 expected = -1;
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 25>(storage, U64{}, &downloaded, &expected),
        false);
    matrix.Output(downloaded, 0);
    matrix.Output(expected, 0);
    U32 detailsAppId = UINT32_MAX;
    char* detailsName = reinterpret_cast<char*>(1);
    fileSize = -1;
    ProductionSteamId owner(kProductionSteamId);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 26>(storage,
            U64{},
            &detailsAppId,
            &detailsName,
            &fileSize,
            &owner),
        false);
    matrix.Output(detailsAppId, U32{});
    matrix.Output(detailsName, static_cast<char*>(nullptr));
    matrix.Output(fileSize, 0);
    matrix.Output(owner.Value(), U64{});
    data.fill(std::byte{0xa5});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 27>(storage,
            U64{},
            data.data(),
            static_cast<I32>(data.size()),
            U32{},
            I32{}),
        0);
    matrix.Zeroed(data);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 28>(storage), 0);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 29>(storage, I32{}), UINT64_MAX);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 30>(storage,
            "file",
            "preview",
            U32{},
            "title",
            "description",
            I32{},
            nullptr,
            I32{}),
        U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 31>(storage, U64{}), UINT64_MAX);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 32>(storage, U64{}, "file"), false);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 33>(storage, U64{}, "preview"), false);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 34>(storage, U64{}, "title"), false);
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 35>(storage, U64{}, "description"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 36>(storage, U64{}, I32{}), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 37>(storage, U64{}, nullptr), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 38>(storage, U64{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 39>(storage, U64{}, U32{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 40>(storage, U64{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 41>(storage, U32{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 42>(storage, U64{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 43>(storage, U32{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 44>(storage, U64{}), U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 45>(storage, U64{}, "change"), false);
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 46>(storage, U64{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 47>(storage, U64{}, false), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 48>(storage, U64{}), U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 49>(storage, ProductionSteamId{}, U32{}, nullptr, nullptr),
        U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 50>(storage,
            I32{},
            "account",
            "video",
            "preview",
            U32{},
            "title",
            "description",
            I32{},
            nullptr),
        U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 51>(storage, U64{}, I32{}), U64{});
    matrix.Equal(CallProductionSlot<SteamRemoteStorage014Abi, 52>(storage, I32{}, U32{}), U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 53>(storage, I32{}, U32{}, U32{}, U32{}, nullptr, nullptr),
        U64{});
    matrix.Equal(
        CallProductionSlot<SteamRemoteStorage014Abi, 54>(storage, U64{}, "location", U32{}),
        U64{});
    return matrix.valid && matrix.calls == 55;
}

bool VerifyClient017SafeSlots(
    void* const client,
    void*& user,
    void*& matchmaking,
    void*& networking,
    void*& storage) {
    SlotMatrix matrix;
    void* const raw = reinterpret_cast<void*>(1);
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 0>(client), 0);
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 1>(client, I32{}), false);
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 2>(client, I32{}), 0);
    I32 pipe = -1;
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 3>(client, &pipe, I32{}), 0);
    matrix.Output(pipe, 0);
    CallProductionSlot<SteamClient017Abi, 4>(client, I32{}, I32{});
    matrix.VoidCall();
    user = CallProductionSlot<SteamClient017Abi, 5>(client, I32{}, I32{}, "SteamUser019");
    matrix.Equal(user != nullptr && user != &productionUser, true);
    CallProductionSlot<SteamClient017Abi, 7>(client, U32{}, U16{});
    matrix.VoidCall();
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 8>(client, I32{}, I32{}, "SteamFriends015"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 9>(client, I32{}, "SteamUtils009"), raw);
    matchmaking = CallProductionSlot<SteamClient017Abi, 10>(client, I32{}, I32{}, "SteamMatchMaking009");
    matrix.Equal(
        matchmaking != nullptr && matchmaking != &productionMatchmaking,
        true);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 11>(client, I32{}, I32{}, "SteamMatchMakingServers002"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 12>(client, I32{}, I32{}, "SteamFriends015"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 13>(client, I32{}, I32{}, "STEAMUSERSTATS_INTERFACE_VERSION011"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 15>(client, I32{}, I32{}, "STEAMAPPS_INTERFACE_VERSION008"),
        raw);
    networking = CallProductionSlot<SteamClient017Abi, 16>(client, I32{}, I32{}, "SteamNetworking005");
    matrix.Equal(
        networking != nullptr && networking != &productionNetworking,
        true);
    storage = CallProductionSlot<SteamClient017Abi, 17>(client,
        I32{},
        I32{},
        "STEAMREMOTESTORAGE_INTERFACE_VERSION014");
    matrix.Equal(storage != nullptr && storage != &productionRemoteStorage, true);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 18>(client, I32{}, I32{}, "STEAMSCREENSHOTS_INTERFACE_VERSION003"),
        raw);
    CallProductionSlot<SteamClient017Abi, 19>(client);
    matrix.VoidCall();
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 20>(client), U32{});
    CallProductionSlot<SteamClient017Abi, 21>(
        client, static_cast<ProductionWarningMessageHook>(nullptr));
    matrix.VoidCall();
    matrix.Equal(CallProductionSlot<SteamClient017Abi, 22>(client), false);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 23>(client, I32{}, I32{}, "STEAMHTTP_INTERFACE_VERSION002"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 24>(client,
            I32{},
            I32{},
            "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 25>(client, I32{}, I32{}, "SteamController005"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 26>(client, I32{}, I32{}, "STEAMUGC_INTERFACE_VERSION010"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 27>(client, I32{}, I32{}, "STEAMAPPLIST_INTERFACE_VERSION001"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 28>(client, I32{}, I32{}, "STEAMMUSIC_INTERFACE_VERSION001"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 29>(client,
            I32{},
            I32{},
            "STEAMMUSICREMOTE_INTERFACE_VERSION001"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 30>(client, I32{}, I32{}, "STEAMHTMLSURFACE_INTERFACE_VERSION_003"),
        raw);
    CallProductionSlot<SteamClient017Abi, 31>(
        client, static_cast<ProductionPostApiResultHook>(nullptr));
    matrix.VoidCall();
    CallProductionSlot<SteamClient017Abi, 32>(
        client, static_cast<ProductionPostApiResultHook>(nullptr));
    matrix.VoidCall();
    CallProductionSlot<SteamClient017Abi, 33>(
        client, static_cast<ProductionCheckCallbackRegisteredHook>(nullptr));
    matrix.VoidCall();
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 34>(client, I32{}, I32{}, "STEAMINVENTORY_INTERFACE_V002"),
        raw);
    matrix.Equal(
        CallProductionSlot<SteamClient017Abi, 35>(client, I32{}, I32{}, "STEAMVIDEO_INTERFACE_V002"),
        raw);
    return matrix.valid && matrix.calls == 34;
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
        "SteamFriends015",
        "SteamUtils009",
        "SteamMatchMakingServers002",
        "STEAMUSERSTATS_INTERFACE_VERSION011",
        "STEAMAPPS_INTERFACE_VERSION008",
        "STEAMSCREENSHOTS_INTERFACE_VERSION003",
        "STEAMHTTP_INTERFACE_VERSION002",
        "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001",
        "SteamController005",
        "STEAMUGC_INTERFACE_VERSION010",
        "STEAMAPPLIST_INTERFACE_VERSION001",
        "STEAMMUSIC_INTERFACE_VERSION001",
        "STEAMMUSICREMOTE_INTERFACE_VERSION001",
        "STEAMHTMLSURFACE_INTERFACE_VERSION_003",
        "STEAMINVENTORY_INTERFACE_V002",
        "STEAMVIDEO_INTERFACE_V002",
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
    void* user = nullptr;
    void* matchmaking = nullptr;
    void* networking = nullptr;
    void* remoteStorage = nullptr;
    const bool clientSlots = VerifyClient017SafeSlots(
        client, user, matchmaking, networking, remoteStorage);
    const bool noRawProtectedObject = client != &productionClient
        && user != &productionUser
        && matchmaking != &productionMatchmaking
        && networking != &productionNetworking
        && remoteStorage != &productionRemoteStorage;
    const bool userSlots = VerifyUser019Slots(user);
    const bool matchmakingSlots = VerifyMatchmaking009Slots(matchmaking);
    const bool networkingSlots = VerifyNetworking005Slots(networking);
    const bool remoteStorageSlots = VerifyRemoteStorage014Slots(remoteStorage);
    const bool exactIdentity = productionRawIdentityCalls == 1;
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

    const auto rawProtectedCallsBeforeGeneric = productionRawProtectedCalls;
    const auto clientGetterCallsBeforeGeneric = productionRawClientGetterCalls;
    void* const genericUser = CallProductionSlot<SteamClient017Abi, 12>(client,
        std::int32_t{},
        std::int32_t{},
        "SteamUser019");
    void* const genericNetworking = CallProductionSlot<SteamClient017Abi, 12>(client,
        std::int32_t{},
        std::int32_t{},
        "SteamNetworking005");
    const bool genericProtectedClientGatewayDenied = genericUser == user
        && genericUser != &productionUser
        && genericNetworking == networking
        && genericNetworking != &productionNetworking
        && !CallProductionSlot<SteamUser019Abi, 1>(genericUser)
        && !CallProductionSlot<SteamNetworking005Abi, 0>(genericNetworking,
            ProductionSteamId{},
            nullptr,
            U32{},
            I32{},
            I32{})
        && productionRawProtectedCalls == rawProtectedCallsBeforeGeneric
        && productionRawClientGetterCalls == clientGetterCallsBeforeGeneric + 2;

    const auto clientGetterCallsBeforeUnknown = productionRawClientGetterCalls;
    const bool unknownClientGatewayDenied = CallProductionSlot<SteamClient017Abi, 6>(client,
            std::int32_t{},
            std::int32_t{},
            "SteamGameServer012") == nullptr
        && productionRawClientGetterCalls == clientGetterCallsBeforeUnknown
        && fatalState->IsFatal();
    fatalState->EnterDenyOnly();
    const auto clientGetterCallsBeforeDeactivated = productionRawClientGetterCalls;
    const bool deactivatedClientGatewayDenied = CallProductionSlot<SteamClient017Abi, 12>(client,
            std::int32_t{},
            std::int32_t{},
            "SteamNetworking005") == nullptr
        && productionRawClientGetterCalls == clientGetterCallsBeforeDeactivated;

    DSRRandomizer::Steam::UnregisterSteamFactorySlot(slot);
    const bool teardownDeny = CallProductionSlot<SteamUser019Abi, 2>(user)
            .Value() == 0
        && !CallProductionSlot<SteamNetworking005Abi, 0>(networking,
            ProductionSteamId{},
            nullptr,
            U32{},
            I32{},
            I32{})
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

    return clientSlots && userSlots && matchmakingSlots && networkingSlots
            && remoteStorageSlots && noRawProtectedObject && exactIdentity
            && exactWindowsSlotCounts
            && protectedRawNeverCalled && genericProtectedClientGatewayDenied
            && unknownClientGatewayDenied
            && deactivatedClientGatewayDenied && teardownDeny
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

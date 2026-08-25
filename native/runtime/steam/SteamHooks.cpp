#include "steam/SteamHooks.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <utility>

#include "steam/SteamPolicy.h"

namespace DSRRandomizer::Steam {
namespace {

struct SlotContext {
    std::vector<std::string> declaredInterfaces;
    std::shared_ptr<FatalState> fatalState;
    std::atomic<Synthetic::FactoryFunction> original{nullptr};
    SteamInterfaceLayout layout = SteamInterfaceLayout::ProductionPinned;
};

struct Wrapper final {
    const void* vtable = nullptr;
    void* raw = nullptr;
    std::string version;
    std::vector<std::string> declaredInterfaces;
    std::shared_ptr<FatalState> fatalState;
    SteamInterfaceLayout layout = SteamInterfaceLayout::ProductionPinned;
};

std::shared_mutex factoryCallbackGate;

bool DenySyntheticMethod(void*) noexcept { return false; }

bool ForwardIdentityMethod(void* const self) noexcept {
    std::shared_lock callbackLock(factoryCallbackGate);
    auto* const wrapper = static_cast<Wrapper*>(self);
    if (wrapper == nullptr || wrapper->fatalState == nullptr
        || wrapper->fatalState->IsFatal() || wrapper->raw == nullptr) {
        return false;
    }
    auto* const raw = static_cast<Synthetic::Interface*>(wrapper->raw);
    if (raw->vtable == nullptr || raw->vtable->Invoke == nullptr) {
        wrapper->fatalState->Trigger("STEAM_IDENTITY_UNAVAILABLE");
        return false;
    }
    const bool result = raw->vtable->Invoke(raw);
    return !wrapper->fatalState->IsFatal() && result;
}

const Synthetic::InterfaceVTable deniedVTable{&DenySyntheticMethod};
const Synthetic::InterfaceVTable identityVTable{&ForwardIdentityMethod};

// ABI provenance: the public Steamworks SDK v1.40 interface declarations at
// rlabrecque/SteamworksSDK commit 117b1282e83d, corroborated slot-for-slot by
// the LGPL Goldberg compatibility headers. Valve's public API documentation
// confirms these exact interface-version strings and return semantics. Only
// ABI facts are encoded here; no SDK source or binary is incorporated.
using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U16 = std::uint16_t;
using I64 = std::int64_t;
using I32 = std::int32_t;
using WarningMessageHookAbi = void(__cdecl*)(I32, const char*);
using PostApiResultHookAbi = void(__cdecl*)();
using CheckCallbackRegisteredHookAbi = U32(__cdecl*)(I32);

struct SteamParamStringArrayAbi;

#pragma pack(push, 1)
class SteamIdAbi final {
public:
    SteamIdAbi() noexcept : value_(0) {}
    explicit SteamIdAbi(const U64 value) noexcept : value_(value) {}

    [[nodiscard]] U64 Value() const noexcept { return value_; }

private:
    U64 value_;
};
#pragma pack(pop)

static_assert(sizeof(SteamIdAbi) == sizeof(U64));

#pragma pack(push, 1)
class GameIdAbi final {
public:
    GameIdAbi() noexcept : value_(0) {}
    explicit GameIdAbi(const U64 value) noexcept : value_(value) {}

private:
    U64 value_;
};
#pragma pack(pop)

struct P2PSessionStateAbi final {
    std::uint8_t connectionActive;
    std::uint8_t connecting;
    std::uint8_t sessionError;
    std::uint8_t usingRelay;
    I32 bytesQueuedForSend;
    I32 packetsQueuedForSend;
    U32 remoteIp;
    U16 remotePort;
};

static_assert(sizeof(GameIdAbi) == sizeof(U64));
static_assert(sizeof(P2PSessionStateAbi) == 20);

template <typename Value>
void ClearOutput(Value* const output) noexcept {
    if (output != nullptr) {
        *output = Value{};
    }
}

void ClearBuffer(void* const output, const U32 length) noexcept {
    if (output != nullptr && length != 0) {
        std::memset(output, 0, length);
    }
}

void ClearBuffer(void* const output, const I32 length) noexcept {
    if (length > 0) {
        ClearBuffer(output, static_cast<U32>(length));
    }
}

void ClearString(char* const output, const I32 length) noexcept {
    if (output != nullptr && length > 0) {
        output[0] = '\0';
    }
}

template <typename Result, typename... Arguments>
Result OfflineZero(Wrapper*, Arguments...) noexcept {
    return Result{};
}

template <typename... Arguments>
bool OfflineFalse(Wrapper*, Arguments...) noexcept {
    return false;
}

template <typename... Arguments>
void OfflineVoid(Wrapper*, Arguments...) noexcept {}

template <typename... Arguments>
const char* OfflineEmptyString(Wrapper*, Arguments...) noexcept {
    return "";
}

template <typename Result, typename... Arguments>
Result OfflineInvalid(Wrapper*, Arguments...) noexcept {
    return std::numeric_limits<Result>::max();
}

bool TryCallRawSteamId(void* const raw, SteamIdAbi& result) noexcept {
    __try {
        auto** const vtable = *static_cast<void***>(raw);
        if (vtable == nullptr || vtable[2] == nullptr) {
            return false;
        }
        const auto method = reinterpret_cast<
            SteamIdAbi(*)(void*) noexcept>(vtable[2]);
        result = method(raw);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

SteamIdAbi ForwardSteamId(Wrapper* const wrapper) noexcept {
    std::shared_lock callbackLock(factoryCallbackGate);
    if (wrapper == nullptr || wrapper->fatalState == nullptr
        || wrapper->fatalState->IsFatal() || wrapper->raw == nullptr) {
        return {};
    }
    SteamIdAbi result;
    if (!TryCallRawSteamId(wrapper->raw, result)) {
        wrapper->fatalState->Trigger("STEAM_IDENTITY_UNAVAILABLE");
        return {};
    }
    return wrapper->fatalState->IsFatal() ? SteamIdAbi{} : result;
}

template <typename Function>
void* Slot(const Function function) noexcept {
    return reinterpret_cast<void*>(function);
}

void* WrapInterface(
    void* raw,
    std::string_view version,
    InterfaceDecision decision,
    const std::vector<std::string>& declaredInterfaces,
    const std::shared_ptr<FatalState>& fatalState,
    SteamInterfaceLayout layout) noexcept;

bool WrapperDeclares(
    const Wrapper& wrapper,
    const std::string_view version) noexcept {
    return std::any_of(
        wrapper.declaredInterfaces.begin(),
        wrapper.declaredInterfaces.end(),
        [version](const std::string& declared) { return declared == version; });
}

bool ValidateClientRequest(
    Wrapper* const wrapper,
    const char* const version,
    InterfaceDecision& decision) noexcept {
    if (wrapper == nullptr || wrapper->fatalState == nullptr
        || wrapper->fatalState->IsFatal() || version == nullptr) {
        return false;
    }
    const std::string_view requested(version);
    const SteamPolicy policy;
    decision = policy.EvaluateInterface(requested);
    if (!WrapperDeclares(*wrapper, requested)) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNDECLARED");
        return false;
    }
    if (decision == InterfaceDecision::UnknownProtectedFatal
        || decision == InterfaceDecision::Unrecognized) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNSUPPORTED");
        return false;
    }
    return true;
}

bool TryCallRawClientGetter(
    void* const raw,
    const std::size_t slot,
    const I32 user,
    const I32 pipe,
    const char* const version,
    void*& result) noexcept {
    __try {
        auto** const vtable = *static_cast<void***>(raw);
        if (vtable == nullptr || vtable[slot] == nullptr) {
            return false;
        }
        const auto method = reinterpret_cast<
            void*(*)(void*, I32, I32, const char*) noexcept>(vtable[slot]);
        result = method(raw, user, pipe, version);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryCallRawClientUtilsGetter(
    void* const raw,
    const std::size_t slot,
    const I32 pipe,
    const char* const version,
    void*& result) noexcept {
    __try {
        auto** const vtable = *static_cast<void***>(raw);
        if (vtable == nullptr || vtable[slot] == nullptr) {
            return false;
        }
        const auto method = reinterpret_cast<
            void*(*)(void*, I32, const char*) noexcept>(vtable[slot]);
        result = method(raw, pipe, version);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* GuardClientResult(
    Wrapper* const wrapper,
    void* const raw,
    const char* const version) noexcept {
    if (wrapper == nullptr || wrapper->fatalState == nullptr
        || wrapper->fatalState->IsFatal() || version == nullptr) {
        return nullptr;
    }
    const std::string_view requested(version);
    const SteamPolicy policy;
    const auto decision = policy.EvaluateInterface(requested);
    if (!WrapperDeclares(*wrapper, requested)) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNDECLARED");
        return nullptr;
    }
    if (decision == InterfaceDecision::UnknownProtectedFatal
        || decision == InterfaceDecision::Unrecognized) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNSUPPORTED");
        return nullptr;
    }
    if (decision == InterfaceDecision::WrapClient) {
        return wrapper;
    }
    if (decision == InterfaceDecision::AllowRaw) {
        return raw;
    }
    return WrapInterface(
        raw,
        requested,
        decision,
        wrapper->declaredInterfaces,
        wrapper->fatalState,
        wrapper->layout);
}

template <std::size_t SlotIndex>
void* ClientInterfaceGetter(
    Wrapper* const wrapper,
    const I32 user,
    const I32 pipe,
    const char* const version) noexcept {
    std::shared_lock callbackLock(factoryCallbackGate);
    if (wrapper == nullptr || wrapper->raw == nullptr
        || wrapper->fatalState == nullptr || wrapper->fatalState->IsFatal()) {
        return nullptr;
    }
    InterfaceDecision decision = InterfaceDecision::Unrecognized;
    if (!ValidateClientRequest(wrapper, version, decision)) {
        return nullptr;
    }
    if (decision == InterfaceDecision::WrapClient) {
        return wrapper;
    }
    void* raw = nullptr;
    if (!TryCallRawClientGetter(
            wrapper->raw, SlotIndex, user, pipe, version, raw)) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNAVAILABLE");
        return nullptr;
    }
    return GuardClientResult(wrapper, raw, version);
}

void* ClientUtilsGetter(
    Wrapper* const wrapper,
    const I32 pipe,
    const char* const version) noexcept {
    std::shared_lock callbackLock(factoryCallbackGate);
    if (wrapper == nullptr || wrapper->raw == nullptr
        || wrapper->fatalState == nullptr || wrapper->fatalState->IsFatal()) {
        return nullptr;
    }
    InterfaceDecision decision = InterfaceDecision::Unrecognized;
    if (!ValidateClientRequest(wrapper, version, decision)) {
        return nullptr;
    }
    if (decision == InterfaceDecision::WrapClient) {
        return wrapper;
    }
    void* raw = nullptr;
    if (!TryCallRawClientUtilsGetter(wrapper->raw, 9, pipe, version, raw)) {
        wrapper->fatalState->Trigger("STEAM_INTERFACE_UNAVAILABLE");
        return nullptr;
    }
    return GuardClientResult(wrapper, raw, version);
}

I32 OfflineCreateLocalUser(Wrapper*, I32* const pipe, I32) noexcept {
    ClearOutput(pipe);
    return 0;
}

I32 OfflineInitiateGameConnection(
    Wrapper*,
    void* const authenticationBlob,
    const I32 authenticationBlobSize,
    SteamIdAbi,
    U32,
    U16,
    bool) noexcept {
    ClearBuffer(authenticationBlob, authenticationBlobSize);
    return 0;
}

bool OfflineUserDataFolder(
    Wrapper*, char* const folder, const I32 folderSize) noexcept {
    ClearString(folder, folderSize);
    return false;
}

I32 OfflineAvailableVoice(
    Wrapper*,
    U32* const compressed,
    U32* const uncompressed,
    U32) noexcept {
    ClearOutput(compressed);
    ClearOutput(uncompressed);
    return 1;
}

I32 OfflineGetVoice(
    Wrapper*,
    const bool wantsCompressed,
    void* const compressed,
    const U32 compressedCapacity,
    U32* const compressedBytes,
    const bool wantsUncompressed,
    void* const uncompressed,
    const U32 uncompressedCapacity,
    U32* const uncompressedBytes,
    U32) noexcept {
    if (wantsCompressed) {
        ClearBuffer(compressed, compressedCapacity);
    }
    if (wantsUncompressed) {
        ClearBuffer(uncompressed, uncompressedCapacity);
    }
    ClearOutput(compressedBytes);
    ClearOutput(uncompressedBytes);
    return 1;
}

I32 OfflineDecompressVoice(
    Wrapper*,
    const void*,
    U32,
    void* const output,
    const U32 outputCapacity,
    U32* const outputBytes,
    U32) noexcept {
    ClearBuffer(output, outputCapacity);
    ClearOutput(outputBytes);
    return 1;
}

U32 OfflineAuthSessionTicket(
    Wrapper*,
    void* const ticket,
    const I32 ticketCapacity,
    U32* const ticketBytes) noexcept {
    ClearBuffer(ticket, ticketCapacity);
    ClearOutput(ticketBytes);
    return 0;
}

I32 OfflineBeginAuthSession(
    Wrapper*, const void*, I32, SteamIdAbi) noexcept {
    return 1;
}

I32 OfflineUserLicense(Wrapper*, SteamIdAbi, U32) noexcept {
    return 1;
}

bool OfflineEncryptedAppTicket(
    Wrapper*,
    void* const ticket,
    const I32 ticketCapacity,
    U32* const ticketBytes) noexcept {
    ClearBuffer(ticket, ticketCapacity);
    ClearOutput(ticketBytes);
    return false;
}

bool OfflineFavoriteGame(
    Wrapper*,
    I32,
    U32* const appId,
    U32* const ip,
    U16* const connectionPort,
    U16* const queryPort,
    U32* const flags,
    U32* const lastPlayed) noexcept {
    ClearOutput(appId);
    ClearOutput(ip);
    ClearOutput(connectionPort);
    ClearOutput(queryPort);
    ClearOutput(flags);
    ClearOutput(lastPlayed);
    return false;
}

bool OfflineLobbyDataByIndex(
    Wrapper*,
    SteamIdAbi,
    I32,
    char* const key,
    const I32 keyCapacity,
    char* const value,
    const I32 valueCapacity) noexcept {
    ClearString(key, keyCapacity);
    ClearString(value, valueCapacity);
    return false;
}

I32 OfflineLobbyChatEntry(
    Wrapper*,
    SteamIdAbi,
    I32,
    SteamIdAbi* const user,
    void* const data,
    const I32 dataCapacity,
    I32* const chatType) noexcept {
    ClearOutput(user);
    ClearBuffer(data, dataCapacity);
    ClearOutput(chatType);
    return 0;
}

bool OfflineLobbyGameServer(
    Wrapper*,
    SteamIdAbi,
    U32* const ip,
    U16* const port,
    SteamIdAbi* const server) noexcept {
    ClearOutput(ip);
    ClearOutput(port);
    ClearOutput(server);
    return false;
}

bool OfflineP2PPacketAvailable(
    Wrapper*, U32* const messageSize, I32) noexcept {
    ClearOutput(messageSize);
    return false;
}

bool OfflineReadP2PPacket(
    Wrapper*,
    void* const destination,
    const U32 capacity,
    U32* const messageSize,
    SteamIdAbi* const remote,
    I32) noexcept {
    ClearBuffer(destination, capacity);
    ClearOutput(messageSize);
    ClearOutput(remote);
    return false;
}

bool OfflineP2PSessionState(
    Wrapper*, SteamIdAbi, P2PSessionStateAbi* const state) noexcept {
    ClearOutput(state);
    return false;
}

bool OfflineSocketDataAvailable(
    Wrapper*, U32, U32* const messageSize) noexcept {
    ClearOutput(messageSize);
    return false;
}

bool OfflineRetrieveSocketData(
    Wrapper*,
    U32,
    void* const destination,
    const U32 capacity,
    U32* const messageSize) noexcept {
    ClearBuffer(destination, capacity);
    ClearOutput(messageSize);
    return false;
}

bool OfflineListenDataAvailable(
    Wrapper*,
    U32,
    U32* const messageSize,
    U32* const socket) noexcept {
    ClearOutput(messageSize);
    ClearOutput(socket);
    return false;
}

bool OfflineRetrieveListenData(
    Wrapper*,
    U32,
    void* const destination,
    const U32 capacity,
    U32* const messageSize,
    U32* const socket) noexcept {
    ClearBuffer(destination, capacity);
    ClearOutput(messageSize);
    ClearOutput(socket);
    return false;
}

bool OfflineSocketInfo(
    Wrapper*,
    U32,
    SteamIdAbi* const remote,
    I32* const status,
    U32* const ip,
    U16* const port) noexcept {
    ClearOutput(remote);
    ClearOutput(status);
    ClearOutput(ip);
    ClearOutput(port);
    return false;
}

bool OfflineListenSocketInfo(
    Wrapper*, U32, U32* const ip, U16* const port) noexcept {
    ClearOutput(ip);
    ClearOutput(port);
    return false;
}

I32 OfflineFileRead(
    Wrapper*, const char*, void* const data, const I32 capacity) noexcept {
    ClearBuffer(data, capacity);
    return 0;
}

bool OfflineFileReadAsyncComplete(
    Wrapper*, U64, void* const data, const U32 capacity) noexcept {
    ClearBuffer(data, capacity);
    return false;
}

const char* OfflineFileNameAndSize(Wrapper*, I32, I32* const size) noexcept {
    ClearOutput(size);
    return "";
}

bool OfflineQuota(
    Wrapper*, U64* const total, U64* const available) noexcept {
    ClearOutput(total);
    ClearOutput(available);
    return false;
}

bool OfflineUgcProgress(
    Wrapper*, U64, I32* const downloaded, I32* const expected) noexcept {
    ClearOutput(downloaded);
    ClearOutput(expected);
    return false;
}

bool OfflineUgcDetails(
    Wrapper*,
    U64,
    U32* const appId,
    char** const name,
    I32* const fileSize,
    SteamIdAbi* const owner) noexcept {
    ClearOutput(appId);
    ClearOutput(name);
    ClearOutput(fileSize);
    ClearOutput(owner);
    return false;
}

I32 OfflineUgcRead(
    Wrapper*,
    U64,
    void* const data,
    const I32 capacity,
    U32,
    I32) noexcept {
    ClearBuffer(data, capacity);
    return 0;
}

const std::array<void*, 36> steamClient017VTable{
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFalse<I32>),
    Slot(&OfflineZero<I32, I32>),
    Slot(&OfflineCreateLocalUser),
    Slot(&OfflineVoid<I32, I32>),
    Slot(&ClientInterfaceGetter<5>),
    Slot(&ClientInterfaceGetter<6>),
    Slot(&OfflineVoid<U32, U16>),
    Slot(&ClientInterfaceGetter<8>),
    Slot(&ClientUtilsGetter),
    Slot(&ClientInterfaceGetter<10>),
    Slot(&ClientInterfaceGetter<11>),
    Slot(&ClientInterfaceGetter<12>),
    Slot(&ClientInterfaceGetter<13>),
    Slot(&ClientInterfaceGetter<14>),
    Slot(&ClientInterfaceGetter<15>),
    Slot(&ClientInterfaceGetter<16>),
    Slot(&ClientInterfaceGetter<17>),
    Slot(&ClientInterfaceGetter<18>),
    Slot(&OfflineVoid<>),
    Slot(&OfflineZero<U32>),
    Slot(&OfflineVoid<WarningMessageHookAbi>),
    Slot(&OfflineFalse<>),
    Slot(&ClientInterfaceGetter<23>),
    Slot(&ClientInterfaceGetter<24>),
    Slot(&ClientInterfaceGetter<25>),
    Slot(&ClientInterfaceGetter<26>),
    Slot(&ClientInterfaceGetter<27>),
    Slot(&ClientInterfaceGetter<28>),
    Slot(&ClientInterfaceGetter<29>),
    Slot(&ClientInterfaceGetter<30>),
    Slot(&OfflineVoid<PostApiResultHookAbi>),
    Slot(&OfflineVoid<PostApiResultHookAbi>),
    Slot(&OfflineVoid<CheckCallbackRegisteredHookAbi>),
    Slot(&ClientInterfaceGetter<34>),
    Slot(&ClientInterfaceGetter<35>),
};

const std::array<void*, 29> steamUser019VTable{
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFalse<>),
    Slot(&ForwardSteamId),
    Slot(&OfflineInitiateGameConnection),
    Slot(&OfflineVoid<U32, U16>),
    Slot(&OfflineVoid<GameIdAbi, I32, const char*>),
    Slot(&OfflineUserDataFolder),
    Slot(&OfflineVoid<>),
    Slot(&OfflineVoid<>),
    Slot(&OfflineAvailableVoice),
    Slot(&OfflineGetVoice),
    Slot(&OfflineDecompressVoice),
    Slot(&OfflineZero<U32>),
    Slot(&OfflineAuthSessionTicket),
    Slot(&OfflineBeginAuthSession),
    Slot(&OfflineVoid<SteamIdAbi>),
    Slot(&OfflineVoid<U32>),
    Slot(&OfflineUserLicense),
    Slot(&OfflineFalse<>),
    Slot(&OfflineVoid<SteamIdAbi, U32, U16>),
    Slot(&OfflineZero<U64, const void*, I32>),
    Slot(&OfflineEncryptedAppTicket),
    Slot(&OfflineZero<I32, I32, bool>),
    Slot(&OfflineZero<I32>),
    Slot(&OfflineZero<U64, const char*>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineFalse<>),
};

const std::array<void*, 38> steamMatchmaking009VTable{
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFavoriteGame),
    Slot(&OfflineZero<I32, U32, U32, U16, U16, U32, U32>),
    Slot(&OfflineFalse<U32, U32, U16, U16, U32>),
    Slot(&OfflineZero<U64>),
    Slot(&OfflineVoid<const char*, const char*, I32>),
    Slot(&OfflineVoid<const char*, I32, I32>),
    Slot(&OfflineVoid<const char*, I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<SteamIdAbi>),
    Slot(&OfflineZero<SteamIdAbi, I32>),
    Slot(&OfflineZero<U64, I32, I32>),
    Slot(&OfflineZero<U64, SteamIdAbi>),
    Slot(&OfflineVoid<SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi, SteamIdAbi>),
    Slot(&OfflineZero<I32, SteamIdAbi>),
    Slot(&OfflineZero<SteamIdAbi, SteamIdAbi, I32>),
    Slot(&OfflineEmptyString<SteamIdAbi, const char*>),
    Slot(&OfflineFalse<SteamIdAbi, const char*, const char*>),
    Slot(&OfflineZero<I32, SteamIdAbi>),
    Slot(&OfflineLobbyDataByIndex),
    Slot(&OfflineFalse<SteamIdAbi, const char*>),
    Slot(&OfflineEmptyString<SteamIdAbi, SteamIdAbi, const char*>),
    Slot(&OfflineVoid<SteamIdAbi, const char*, const char*>),
    Slot(&OfflineFalse<SteamIdAbi, const void*, I32>),
    Slot(&OfflineLobbyChatEntry),
    Slot(&OfflineFalse<SteamIdAbi>),
    Slot(&OfflineVoid<SteamIdAbi, U32, U16, SteamIdAbi>),
    Slot(&OfflineLobbyGameServer),
    Slot(&OfflineFalse<SteamIdAbi, I32>),
    Slot(&OfflineZero<I32, SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi, I32>),
    Slot(&OfflineFalse<SteamIdAbi, bool>),
    Slot(&OfflineZero<SteamIdAbi, SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi, SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi, SteamIdAbi>),
};

const std::array<void*, 22> steamNetworking005VTable{
    Slot(&OfflineFalse<SteamIdAbi, const void*, U32, I32, I32>),
    Slot(&OfflineP2PPacketAvailable),
    Slot(&OfflineReadP2PPacket),
    Slot(&OfflineFalse<SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi>),
    Slot(&OfflineFalse<SteamIdAbi, I32>),
    Slot(&OfflineP2PSessionState),
    Slot(&OfflineFalse<bool>),
    Slot(&OfflineZero<U32, I32, U32, U16, bool>),
    Slot(&OfflineZero<U32, SteamIdAbi, I32, I32, bool>),
    Slot(&OfflineZero<U32, U32, U16, I32>),
    Slot(&OfflineFalse<U32, bool>),
    Slot(&OfflineFalse<U32, bool>),
    Slot(&OfflineFalse<U32, void*, U32, bool>),
    Slot(&OfflineSocketDataAvailable),
    Slot(&OfflineRetrieveSocketData),
    Slot(&OfflineListenDataAvailable),
    Slot(&OfflineRetrieveListenData),
    Slot(&OfflineSocketInfo),
    Slot(&OfflineListenSocketInfo),
    Slot(&OfflineZero<I32, U32>),
    Slot(&OfflineZero<I32, U32>),
};

const std::array<void*, 55> steamRemoteStorage014VTable{
    Slot(&OfflineFalse<const char*, const void*, I32>),
    Slot(&OfflineFileRead),
    Slot(&OfflineZero<U64, const char*, const void*, U32>),
    Slot(&OfflineZero<U64, const char*, U32, U32>),
    Slot(&OfflineFileReadAsyncComplete),
    Slot(&OfflineFalse<const char*>),
    Slot(&OfflineFalse<const char*>),
    Slot(&OfflineZero<U64, const char*>),
    Slot(&OfflineFalse<const char*, I32>),
    Slot(&OfflineInvalid<U64, const char*>),
    Slot(&OfflineFalse<U64, const void*, I32>),
    Slot(&OfflineFalse<U64>),
    Slot(&OfflineFalse<U64>),
    Slot(&OfflineFalse<const char*>),
    Slot(&OfflineFalse<const char*>),
    Slot(&OfflineZero<I32, const char*>),
    Slot(&OfflineZero<I64, const char*>),
    Slot(&OfflineZero<I32, const char*>),
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFileNameAndSize),
    Slot(&OfflineQuota),
    Slot(&OfflineFalse<>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineVoid<bool>),
    Slot(&OfflineZero<U64, U64, U32>),
    Slot(&OfflineUgcProgress),
    Slot(&OfflineUgcDetails),
    Slot(&OfflineUgcRead),
    Slot(&OfflineZero<I32>),
    Slot(&OfflineInvalid<U64, I32>),
    Slot(&OfflineZero<U64, const char*, const char*, U32, const char*, const char*, I32, SteamParamStringArrayAbi*, I32>),
    Slot(&OfflineInvalid<U64, U64>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, I32>),
    Slot(&OfflineFalse<U64, SteamParamStringArrayAbi*>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineZero<U64, U64, U32>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineZero<U64, U32>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineZero<U64, U32>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineZero<U64, U64, bool>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineZero<U64, SteamIdAbi, U32, SteamParamStringArrayAbi*, SteamParamStringArrayAbi*>),
    Slot(&OfflineZero<U64, I32, const char*, const char*, const char*, U32, const char*, const char*, I32, SteamParamStringArrayAbi*>),
    Slot(&OfflineZero<U64, U64, I32>),
    Slot(&OfflineZero<U64, I32, U32>),
    Slot(&OfflineZero<U64, I32, U32, U32, U32, SteamParamStringArrayAbi*, SteamParamStringArrayAbi*>),
    Slot(&OfflineZero<U64, U64, const char*, U32>),
};

const void* ProductionVTable(const std::string_view version) noexcept {
    if (version == "SteamClient017") {
        return steamClient017VTable.data();
    }
    if (version == "SteamUser019") {
        return steamUser019VTable.data();
    }
    if (version == "SteamMatchMaking009") {
        return steamMatchmaking009VTable.data();
    }
    if (version == "SteamNetworking005") {
        return steamNetworking005VTable.data();
    }
    if (version == "STEAMREMOTESTORAGE_INTERFACE_VERSION014") {
        return steamRemoteStorage014VTable.data();
    }
    return nullptr;
}

std::size_t ProductionSlotCount(const std::string_view version) noexcept {
    if (version == "SteamClient017") {
        return steamClient017VTable.size();
    }
    if (version == "SteamUser019") {
        return steamUser019VTable.size();
    }
    if (version == "SteamMatchMaking009") {
        return steamMatchmaking009VTable.size();
    }
    if (version == "SteamNetworking005") {
        return steamNetworking005VTable.size();
    }
    if (version == "STEAMREMOTESTORAGE_INTERFACE_VERSION014") {
        return steamRemoteStorage014VTable.size();
    }
    return 0;
}
std::array<std::shared_ptr<SlotContext>, kSteamFactorySlotCapacity> slots{};
std::mutex slotsMutex;
std::vector<std::unique_ptr<Wrapper>> processLifetimeWrappers;
std::mutex wrapperMutex;

bool IsDeclared(
    const SlotContext& context,
    const std::string_view version) noexcept {
    return std::any_of(
        context.declaredInterfaces.begin(),
        context.declaredInterfaces.end(),
        [version](const std::string& declared) { return declared == version; });
}

void* FatalAndNull(
    const std::shared_ptr<SlotContext>& context,
    const char* const code) noexcept {
    if (context != nullptr && context->fatalState != nullptr) {
        context->fatalState->Trigger(code);
    }
    return nullptr;
}

void* WrapInterface(
    void* const raw,
    const std::string_view version,
    const InterfaceDecision decision,
    const std::vector<std::string>& declaredInterfaces,
    const std::shared_ptr<FatalState>& fatalState,
    const SteamInterfaceLayout layout) noexcept {
    if (raw == nullptr) {
        return nullptr;
    }
    try {
        std::scoped_lock lock(wrapperMutex);
        const auto found = std::find_if(
            processLifetimeWrappers.begin(),
            processLifetimeWrappers.end(),
            [raw, version, &fatalState](const std::unique_ptr<Wrapper>& wrapper) {
                return wrapper->raw == raw && wrapper->version == version
                    && wrapper->fatalState == fatalState;
            });
        if (found != processLifetimeWrappers.end()) {
            return found->get();
        }
        auto wrapper = std::make_unique<Wrapper>();
        wrapper->vtable = layout == SteamInterfaceLayout::ProductionPinned
            ? ProductionVTable(version)
            : static_cast<const void*>(
                decision == InterfaceDecision::AllowOwnershipIdentity
                    ? &identityVTable
                    : &deniedVTable);
        if (wrapper->vtable == nullptr) {
            return nullptr;
        }
        wrapper->raw = raw;
        wrapper->version.assign(version);
        wrapper->declaredInterfaces = declaredInterfaces;
        wrapper->fatalState = fatalState;
        wrapper->layout = layout;
        auto* const result = wrapper.get();
        processLifetimeWrappers.push_back(std::move(wrapper));
        return result;
    }
    catch (...) {
        return nullptr;
    }
}

void* GuardedFactory(
    const std::size_t slot,
    const char* const version) noexcept {
    std::shared_lock callbackLock(factoryCallbackGate);
    std::shared_ptr<SlotContext> context;
    {
        std::scoped_lock lock(slotsMutex);
        if (slot < slots.size()) {
            context = slots[slot];
        }
    }
    if (context == nullptr || version == nullptr) {
        return FatalAndNull(context, "STEAM_INTERFACE_UNSUPPORTED");
    }
    if (context->fatalState == nullptr || context->fatalState->IsFatal()) {
        return nullptr;
    }

    const std::string_view requested(version);
    const SteamPolicy policy;
    const auto decision = policy.EvaluateInterface(requested);
    if (decision == InterfaceDecision::UnknownProtectedFatal) {
        return FatalAndNull(context, "STEAM_INTERFACE_UNSUPPORTED");
    }
    if (decision == InterfaceDecision::Unrecognized
        || !IsDeclared(*context, requested)) {
        return FatalAndNull(context, "STEAM_INTERFACE_UNDECLARED");
    }

    const auto original = context->original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return FatalAndNull(context, "STEAM_FACTORY_UNAVAILABLE");
    }
    void* const raw = original(version);
    if (context->fatalState->IsFatal()) {
        return nullptr;
    }
    if (decision == InterfaceDecision::AllowRaw) {
        return raw;
    }
    void* const wrapped = WrapInterface(
        raw,
        requested,
        decision,
        context->declaredInterfaces,
        context->fatalState,
        context->layout);
    if (context->fatalState->IsFatal()) {
        return nullptr;
    }
    return wrapped != nullptr || raw == nullptr
        ? wrapped
        : FatalAndNull(context, "STEAM_WRAPPER_UNAVAILABLE");
}

#define DSR_STEAM_FACTORY_DETOUR(index)                                      \
    void* __cdecl FactoryDetour##index(const char* const version) noexcept { \
        return GuardedFactory(index, version);                               \
    }

DSR_STEAM_FACTORY_DETOUR(0)
DSR_STEAM_FACTORY_DETOUR(1)
DSR_STEAM_FACTORY_DETOUR(2)
DSR_STEAM_FACTORY_DETOUR(3)
DSR_STEAM_FACTORY_DETOUR(4)
DSR_STEAM_FACTORY_DETOUR(5)
DSR_STEAM_FACTORY_DETOUR(6)
DSR_STEAM_FACTORY_DETOUR(7)

#undef DSR_STEAM_FACTORY_DETOUR

const std::array<void*, kSteamFactorySlotCapacity> detours{
    reinterpret_cast<void*>(&FactoryDetour0),
    reinterpret_cast<void*>(&FactoryDetour1),
    reinterpret_cast<void*>(&FactoryDetour2),
    reinterpret_cast<void*>(&FactoryDetour3),
    reinterpret_cast<void*>(&FactoryDetour4),
    reinterpret_cast<void*>(&FactoryDetour5),
    reinterpret_cast<void*>(&FactoryDetour6),
    reinterpret_cast<void*>(&FactoryDetour7),
};

}  // namespace

FatalState::FatalState(const FatalReporter reporter) noexcept
    : reporter_(reporter) {}

void FatalState::EnterDenyOnly() noexcept {
    fatal_.store(true, std::memory_order_release);
}

void FatalState::Trigger(const char* const code) noexcept {
    EnterDenyOnly();
    if (reporter_ != nullptr) {
        reporter_(code);
    }
}

bool FatalState::IsFatal() const noexcept {
    return fatal_.load(std::memory_order_acquire);
}

class SteamFactoryCallbackBlock::Impl final {
public:
    Impl() noexcept : lock(factoryCallbackGate) {}

private:
    std::unique_lock<std::shared_mutex> lock;
};

SteamFactoryCallbackBlock::SteamFactoryCallbackBlock() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    }
    catch (...) {
        std::terminate();
    }
}

SteamFactoryCallbackBlock::~SteamFactoryCallbackBlock() = default;

SteamFactorySlotStatus RegisterSteamFactorySlot(
    const std::size_t slot,
    const std::vector<std::string>& declaredInterfaces,
    const std::shared_ptr<FatalState>& fatalState,
    const SteamInterfaceLayout layout) noexcept {
    if (slot >= slots.size() || declaredInterfaces.empty()
        || fatalState == nullptr) {
        return SteamFactorySlotStatus::InvalidConfiguration;
    }
    try {
        const SteamPolicy policy;
        for (const auto& version : declaredInterfaces) {
            const auto decision = policy.EvaluateInterface(version);
            if (version.empty()
                || decision == InterfaceDecision::UnknownProtectedFatal
                || decision == InterfaceDecision::Unrecognized
                || (layout == SteamInterfaceLayout::ProductionPinned
                    && decision != InterfaceDecision::AllowRaw
                    && ProductionVTable(version) == nullptr)) {
                return SteamFactorySlotStatus::InvalidConfiguration;
            }
        }
        auto candidate = std::make_shared<SlotContext>();
        candidate->declaredInterfaces = declaredInterfaces;
        candidate->fatalState = fatalState;
        candidate->layout = layout;
        std::scoped_lock lock(slotsMutex);
        if (slots[slot] != nullptr) {
            return SteamFactorySlotStatus::SlotUnavailable;
        }
        slots[slot] = std::move(candidate);
        return SteamFactorySlotStatus::Success;
    }
    catch (...) {
        return SteamFactorySlotStatus::InvalidConfiguration;
    }
}

bool SetSteamFactoryOriginal(
    const std::size_t slot,
    const Synthetic::FactoryFunction original) noexcept {
    std::scoped_lock lock(slotsMutex);
    if (slot >= slots.size() || slots[slot] == nullptr || original == nullptr) {
        return false;
    }
    slots[slot]->original.store(original, std::memory_order_release);
    return true;
}

void* SteamFactoryDetourAddress(const std::size_t slot) noexcept {
    return slot < detours.size() ? detours[slot] : nullptr;
}

void UnregisterSteamFactorySlot(const std::size_t slot) noexcept {
    std::scoped_lock lock(slotsMutex);
    if (slot < slots.size()) {
        slots[slot].reset();
    }
}

namespace Testing {

std::size_t ProductionInterfaceSlotCount(
    const std::string_view version) noexcept {
    return ProductionSlotCount(version);
}

}  // namespace Testing

}  // namespace DSRRandomizer::Steam

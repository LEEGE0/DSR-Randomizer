#include "steam/SteamHooks.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
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

const std::array<void*, 36> steamClient017VTable{
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFalse<I32>),
    Slot(&OfflineZero<I32, I32>),
    Slot(&OfflineZero<I32, I32*, I32>),
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
    Slot(&OfflineVoid<void*>),
    Slot(&OfflineFalse<>),
    Slot(&ClientInterfaceGetter<23>),
    Slot(&ClientInterfaceGetter<24>),
    Slot(&ClientInterfaceGetter<25>),
    Slot(&ClientInterfaceGetter<26>),
    Slot(&ClientInterfaceGetter<27>),
    Slot(&ClientInterfaceGetter<28>),
    Slot(&ClientInterfaceGetter<29>),
    Slot(&ClientInterfaceGetter<30>),
    Slot(&OfflineVoid<void*>),
    Slot(&OfflineVoid<void*>),
    Slot(&OfflineVoid<void*>),
    Slot(&ClientInterfaceGetter<34>),
    Slot(&ClientInterfaceGetter<35>),
};

const std::array<void*, 29> steamUser019VTable{
    Slot(&OfflineZero<I32>),
    Slot(&OfflineFalse<>),
    Slot(&ForwardSteamId),
    Slot(&OfflineZero<I32, void*, I32, U64, U32, U16, bool>),
    Slot(&OfflineVoid<U32, U16>),
    Slot(&OfflineVoid<U64, I32, const char*>),
    Slot(&OfflineFalse<char*, I32>),
    Slot(&OfflineVoid<>),
    Slot(&OfflineVoid<>),
    Slot(&OfflineZero<I32, U32*, U32*, U32>),
    Slot(&OfflineZero<I32, bool, void*, U32, U32*, bool, void*, U32, U32*, U32>),
    Slot(&OfflineZero<I32, const void*, U32, void*, U32, U32*, U32>),
    Slot(&OfflineZero<U32>),
    Slot(&OfflineZero<U32, void*, I32, U32*>),
    Slot(&OfflineZero<I32, const void*, I32, U64>),
    Slot(&OfflineVoid<U64>),
    Slot(&OfflineVoid<U32>),
    Slot(&OfflineZero<I32, U64, U32>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineVoid<U64, U32, U16>),
    Slot(&OfflineZero<U64, void*, I32>),
    Slot(&OfflineFalse<void*, I32, U32*>),
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
    Slot(&OfflineFalse<I32, U32*, U32*, U16*, U16*, U32*, U32*>),
    Slot(&OfflineZero<I32, U32, U32, U16, U16, U32, U32>),
    Slot(&OfflineFalse<U32, U32, U16, U16, U32>),
    Slot(&OfflineZero<U64>),
    Slot(&OfflineVoid<const char*, const char*, I32>),
    Slot(&OfflineVoid<const char*, I32, I32>),
    Slot(&OfflineVoid<const char*, I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<I32>),
    Slot(&OfflineVoid<U64>),
    Slot(&OfflineZero<SteamIdAbi, I32>),
    Slot(&OfflineZero<U64, I32, I32>),
    Slot(&OfflineZero<U64, U64>),
    Slot(&OfflineVoid<U64>),
    Slot(&OfflineFalse<U64, U64>),
    Slot(&OfflineZero<I32, U64>),
    Slot(&OfflineZero<SteamIdAbi, U64, I32>),
    Slot(&OfflineEmptyString<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*, const char*>),
    Slot(&OfflineZero<I32, U64>),
    Slot(&OfflineFalse<U64, I32, char*, I32, char*, I32>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineEmptyString<U64, U64, const char*>),
    Slot(&OfflineVoid<U64, const char*, const char*>),
    Slot(&OfflineFalse<U64, const void*, I32>),
    Slot(&OfflineZero<I32, U64, I32, U64*, void*, I32, I32*>),
    Slot(&OfflineFalse<U64>),
    Slot(&OfflineVoid<U64, U32, U16, U64>),
    Slot(&OfflineFalse<U64, U32*, U16*, U64*>),
    Slot(&OfflineFalse<U64, I32>),
    Slot(&OfflineZero<I32, U64>),
    Slot(&OfflineFalse<U64, I32>),
    Slot(&OfflineFalse<U64, bool>),
    Slot(&OfflineZero<SteamIdAbi, U64>),
    Slot(&OfflineFalse<U64, U64>),
    Slot(&OfflineFalse<U64, U64>),
};

const std::array<void*, 22> steamNetworking005VTable{
    Slot(&OfflineFalse<U64, const void*, U32, I32, I32>),
    Slot(&OfflineFalse<U32*, I32>),
    Slot(&OfflineFalse<void*, U32, U32*, U64*, I32>),
    Slot(&OfflineFalse<U64>),
    Slot(&OfflineFalse<U64>),
    Slot(&OfflineFalse<U64, I32>),
    Slot(&OfflineFalse<U64, void*>),
    Slot(&OfflineFalse<bool>),
    Slot(&OfflineZero<U32, I32, U32, U16, bool>),
    Slot(&OfflineZero<U32, U64, I32, I32, bool>),
    Slot(&OfflineZero<U32, U32, U16, I32>),
    Slot(&OfflineFalse<U32, bool>),
    Slot(&OfflineFalse<U32, bool>),
    Slot(&OfflineFalse<U32, void*, U32, bool>),
    Slot(&OfflineFalse<U32, U32*>),
    Slot(&OfflineFalse<U32, void*, U32, U32*>),
    Slot(&OfflineFalse<U32, U32*, U32*>),
    Slot(&OfflineFalse<U32, void*, U32, U32*, U32*>),
    Slot(&OfflineFalse<U32, U64*, I32*, U32*, U16*>),
    Slot(&OfflineFalse<U32, U32*, U16*>),
    Slot(&OfflineZero<I32, U32>),
    Slot(&OfflineZero<I32, U32>),
};

const std::array<void*, 55> steamRemoteStorage014VTable{
    Slot(&OfflineFalse<const char*, const void*, I32>),
    Slot(&OfflineZero<I32, const char*, void*, I32>),
    Slot(&OfflineZero<U64, const char*, const void*, U32>),
    Slot(&OfflineZero<U64, const char*, U32, U32>),
    Slot(&OfflineFalse<U64, void*, U32>),
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
    Slot(&OfflineEmptyString<I32, I32*>),
    Slot(&OfflineFalse<U64*, U64*>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineFalse<>),
    Slot(&OfflineVoid<bool>),
    Slot(&OfflineZero<U64, U64, U32>),
    Slot(&OfflineFalse<U64, I32*, I32*>),
    Slot(&OfflineFalse<U64, U32*, char**, I32*, U64*>),
    Slot(&OfflineZero<I32, U64, void*, I32, U32, I32>),
    Slot(&OfflineZero<I32>),
    Slot(&OfflineInvalid<U64, I32>),
    Slot(&OfflineZero<U64, const char*, const char*, U32, const char*, const char*, I32, void*, I32>),
    Slot(&OfflineInvalid<U64, U64>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, const char*>),
    Slot(&OfflineFalse<U64, I32>),
    Slot(&OfflineFalse<U64, void*>),
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
    Slot(&OfflineZero<U64, U64, U32, void*, void*>),
    Slot(&OfflineZero<U64, I32, const char*, const char*, const char*, U32, const char*, const char*, I32, void*>),
    Slot(&OfflineZero<U64, U64, I32>),
    Slot(&OfflineZero<U64, I32, U32>),
    Slot(&OfflineZero<U64, I32, U32, U32, U32, void*, void*>),
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

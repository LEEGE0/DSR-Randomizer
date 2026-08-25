#include "steam/SteamHooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
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
};

struct Wrapper final {
    const Synthetic::InterfaceVTable* vtable = nullptr;
    void* raw = nullptr;
    std::string version;
    std::shared_ptr<FatalState> fatalState;
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
    const std::shared_ptr<FatalState>& fatalState) noexcept {
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
        wrapper->vtable = decision == InterfaceDecision::AllowOwnershipIdentity
            ? &identityVTable
            : &deniedVTable;
        wrapper->raw = raw;
        wrapper->version.assign(version);
        wrapper->fatalState = fatalState;
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
    void* const wrapped = WrapInterface(
        raw,
        requested,
        decision,
        context->fatalState);
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
    const std::shared_ptr<FatalState>& fatalState) noexcept {
    if (slot >= slots.size() || declaredInterfaces.empty()
        || fatalState == nullptr) {
        return SteamFactorySlotStatus::InvalidConfiguration;
    }
    try {
        const SteamPolicy policy;
        for (const auto& version : declaredInterfaces) {
            if (version.empty()
                || policy.EvaluateInterface(version)
                    == InterfaceDecision::UnknownProtectedFatal
                || policy.EvaluateInterface(version)
                    == InterfaceDecision::Unrecognized) {
                return SteamFactorySlotStatus::InvalidConfiguration;
            }
        }
        auto candidate = std::make_shared<SlotContext>();
        candidate->declaredInterfaces = declaredInterfaces;
        candidate->fatalState = fatalState;
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

}  // namespace DSRRandomizer::Steam

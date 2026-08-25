#include <Windows.h>

#include <atomic>
#include <cstring>

#include "steam/SteamHooks.h"

namespace {

std::atomic<std::uint32_t> protectedCalls{0};
std::atomic<std::uint32_t> identityCalls{0};
std::atomic<DWORD> unexpectedFactoryExitCode{0};

bool InvokeProtected(void*) noexcept {
    protectedCalls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool InvokeIdentity(void*) noexcept {
    identityCalls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const DSRRandomizer::Steam::Synthetic::InterfaceVTable protectedVTable{
    &InvokeProtected,
};
const DSRRandomizer::Steam::Synthetic::InterfaceVTable identityVTable{
    &InvokeIdentity,
};
DSRRandomizer::Steam::Synthetic::Interface protectedInterface{&protectedVTable};
DSRRandomizer::Steam::Synthetic::Interface identityInterface{&identityVTable};

bool IsKnownVersion(const char* const version) noexcept {
    return version != nullptr
        && (std::strcmp(version, "SteamMatchMaking009") == 0
            || std::strcmp(version, "SteamNetworking006") == 0
            || std::strcmp(
                version,
                "STEAMREMOTESTORAGE_INTERFACE_VERSION016") == 0
            || std::strcmp(version, "SteamUser023") == 0);
}

}  // namespace

extern "C" __declspec(dllexport) void* __cdecl FakeSteamFactory(
    const char* const version) noexcept {
    const auto unexpectedExit = unexpectedFactoryExitCode.load(
        std::memory_order_relaxed);
    if (!IsKnownVersion(version) && unexpectedExit != 0) {
        ExitProcess(unexpectedExit);
    }
    return version != nullptr && std::strcmp(version, "SteamUser023") == 0
        ? static_cast<void*>(&identityInterface)
        : static_cast<void*>(&protectedInterface);
}

extern "C" __declspec(dllexport) void __cdecl FakeSteamResetCounters() noexcept {
    protectedCalls.store(0, std::memory_order_relaxed);
    identityCalls.store(0, std::memory_order_relaxed);
    unexpectedFactoryExitCode.store(0, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
FakeSteamProtectedCallCount() noexcept {
    return protectedCalls.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
FakeSteamIdentityCallCount() noexcept {
    return identityCalls.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) void __cdecl
FakeSteamSetUnexpectedFactoryExitCode(const DWORD value) noexcept {
    unexpectedFactoryExitCode.store(value, std::memory_order_relaxed);
}

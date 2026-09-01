#include "monitor/HookIntegrityRegistry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

#include "hooks/MinHookCoordinator.h"

namespace DSRRandomizer::Monitor {
namespace {

struct IntegrityEntry {
    void* target = nullptr;
    void* trampoline = nullptr;
    std::size_t declaredPatchBytes = 0;
    std::array<std::byte, kMaximumDeclaredPatchBytes> targetBytes{};
    std::array<std::byte, kMaximumDeclaredPatchBytes> trampolineBytes{};
};

std::mutex registryMutex;
std::vector<IntegrityEntry> entries;

bool ReadBytes(
    const void* const address,
    const std::size_t length,
    std::byte* const destination) noexcept {
    if (address == nullptr || destination == nullptr || length == 0
        || length > kMaximumDeclaredPatchBytes) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(
            GetCurrentProcess(),
            address,
            destination,
            length,
            &read)
        && read == length;
}

}  // namespace

bool RegisterInstalledHook(
    void* const target,
    void* const trampoline,
    const std::size_t declaredPatchBytes) noexcept {
    if (target == nullptr || trampoline == nullptr || declaredPatchBytes == 0
        || declaredPatchBytes > kMaximumDeclaredPatchBytes) {
        return false;
    }

    try {
        Hooks::MinHookMutationLease mutation;
        IntegrityEntry entry{};
        entry.target = target;
        entry.trampoline = trampoline;
        entry.declaredPatchBytes = declaredPatchBytes;
        if (!ReadBytes(
                target,
                declaredPatchBytes,
                entry.targetBytes.data())
            || !ReadBytes(
                trampoline,
                declaredPatchBytes,
                entry.trampolineBytes.data())) {
            return false;
        }
        std::scoped_lock lock(registryMutex);
        const auto existing = std::find_if(
            entries.begin(), entries.end(),
            [target](const IntegrityEntry& candidate) {
                return candidate.target == target;
            });
        if (existing != entries.end()) {
            *existing = entry;
        }
        else {
            entries.push_back(entry);
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

void UnregisterInstalledHook(void* const target) noexcept {
    Hooks::MinHookMutationLease mutation;
    std::scoped_lock lock(registryMutex);
    entries.erase(
        std::remove_if(
            entries.begin(), entries.end(),
            [target](const IntegrityEntry& candidate) {
                return candidate.target == target;
            }),
        entries.end());
}

bool VerifyAllInstalledHooks() noexcept {
    Hooks::MinHookMutationLease mutation;
    std::scoped_lock lock(registryMutex);
    std::array<std::byte, kMaximumDeclaredPatchBytes> current{};
    for (const auto& entry : entries) {
        if (!ReadBytes(
                entry.target,
                entry.declaredPatchBytes,
                current.data())
            || !std::equal(
                current.begin(),
                current.begin() + static_cast<std::ptrdiff_t>(
                    entry.declaredPatchBytes),
                entry.targetBytes.begin())
            || !ReadBytes(
                entry.trampoline,
                entry.declaredPatchBytes,
                current.data())
            || !std::equal(
                current.begin(),
                current.begin() + static_cast<std::ptrdiff_t>(
                    entry.declaredPatchBytes),
                entry.trampolineBytes.begin())) {
            return false;
        }
    }
    return true;
}

std::size_t InstalledHookCount() noexcept {
    Hooks::MinHookMutationLease mutation;
    std::scoped_lock lock(registryMutex);
    return entries.size();
}

void ClearHookIntegrityRegistry() noexcept {
    Hooks::MinHookMutationLease mutation;
    std::scoped_lock lock(registryMutex);
    entries.clear();
}

void ClearHookIntegrityRegistryForTesting() noexcept {
    ClearHookIntegrityRegistry();
}

}  // namespace DSRRandomizer::Monitor

#pragma once

#include <cstddef>

namespace DSRRandomizer::Monitor {

inline constexpr std::size_t kMaximumDeclaredPatchBytes = 32;
inline constexpr std::size_t kDefaultDeclaredPatchBytes = 16;

[[nodiscard]] bool RegisterInstalledHook(
    void* target,
    void* trampoline,
    std::size_t declaredPatchBytes = kDefaultDeclaredPatchBytes) noexcept;
void UnregisterInstalledHook(void* target) noexcept;
[[nodiscard]] bool VerifyAllInstalledHooks() noexcept;
[[nodiscard]] std::size_t InstalledHookCount() noexcept;
void ClearHookIntegrityRegistry() noexcept;
void ClearHookIntegrityRegistryForTesting() noexcept;

}  // namespace DSRRandomizer::Monitor

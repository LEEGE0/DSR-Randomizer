#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "steam/SteamHooks.h"

namespace DSRRandomizer::Modules {

struct DeferredModuleExpectation {
    std::wstring expectedPath;
    std::array<std::uint8_t, 32> expectedSha256{};
    bool allowDeferred = false;
    std::vector<std::string> declaredInterfaces;
    std::vector<std::string> protectedFactoryExports;
};

struct DeferredModuleGateConfiguration {
    std::vector<DeferredModuleExpectation> modules;
    Steam::FatalReporter fatalReporter = nullptr;
};

enum class DeferredModuleGateInstallStatus {
    Success,
    InvalidConfiguration,
    AdmissionFailed,
    HookInstallFailed,
};

enum class DeferredModuleGateCleanupStatus {
    Success,
    Incomplete,
};

[[nodiscard]] DeferredModuleGateInstallStatus InstallDeferredModuleGate(
    const DeferredModuleGateConfiguration& configuration) noexcept;
[[nodiscard]] DeferredModuleGateCleanupStatus UninstallDeferredModuleGate() noexcept;
[[nodiscard]] bool DeferredModuleGateIsInstalled() noexcept;

}  // namespace DSRRandomizer::Modules

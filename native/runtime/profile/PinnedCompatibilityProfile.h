#pragma once

#include <memory>

#include "game/GameServiceGuard.h"
#include "modules/DeferredModuleGate.h"
#include "save/SaveCallsiteRedirect.h"

namespace DSRRandomizer::Profile {

struct PinnedCompatibilityProfile {
    Game::GameServiceGuardConfiguration gameService;
    Modules::DeferredModuleGateConfiguration steam;
    Save::SaveCallsiteRedirectConfiguration saveRedirect;
    std::shared_ptr<void> identityLease;
};

enum class PinnedCompatibilityProfileStatus {
    Success,
    ProfileMismatch,
    InvalidConfiguration,
};

[[nodiscard]] PinnedCompatibilityProfileStatus
BuildPinnedCompatibilityProfile(PinnedCompatibilityProfile& profile) noexcept;

}  // namespace DSRRandomizer::Profile

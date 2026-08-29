#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace DSRRandomizer::Save {

inline constexpr std::size_t kSaveCallsiteFingerprintSize = 14;

struct SaveCallsiteRedirectTarget {
    std::byte* address = nullptr;
    std::array<std::uint8_t, kSaveCallsiteFingerprintSize> expected{};
    std::size_t callOffset = 0;
};

struct SaveCallsiteRedirectConfiguration {
    std::wstring dedicatedRmm;
    std::array<SaveCallsiteRedirectTarget, 2> targets{};
    std::shared_ptr<void> identityLease;
};

enum class SaveCallsiteRedirectInstallStatus {
    Success,
    InvalidConfiguration,
    ProfileMismatch,
    PatchFailed,
};

enum class SaveCallsiteRedirectCleanupStatus {
    Success,
    Incomplete,
};

[[nodiscard]] SaveCallsiteRedirectInstallStatus InstallSaveCallsiteRedirect(
    const SaveCallsiteRedirectConfiguration& configuration) noexcept;
[[nodiscard]] SaveCallsiteRedirectCleanupStatus
UninstallSaveCallsiteRedirect() noexcept;
[[nodiscard]] bool SaveCallsiteRedirectIsInstalled() noexcept;

HANDLE WINAPI RedirectSaveCreateFileW(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile);

}  // namespace DSRRandomizer::Save

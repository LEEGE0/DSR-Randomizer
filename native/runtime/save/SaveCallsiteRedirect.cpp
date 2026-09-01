#include "save/SaveCallsiteRedirect.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>

extern "C" HANDLE WINAPI DsrRedirectSaveCreateFileW(
    LPCWSTR,
    DWORD,
    DWORD,
    LPSECURITY_ATTRIBUTES,
    DWORD,
    DWORD,
    HANDLE);

namespace DSRRandomizer::Save {
namespace {

struct RedirectState {
    std::wstring dedicatedRmm;
};

struct RedirectLifecycle {
    void** importSlot = nullptr;
    void* originalImport = nullptr;
};

std::shared_mutex redirectGate;
std::shared_ptr<const RedirectState> activeState;
RedirectLifecycle lifecycle{};
bool installed = false;

bool EqualsOrdinalIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

bool HasExactNormalSaveName(const wchar_t* path) noexcept {
    if (path == nullptr) {
        return false;
    }
    const std::wstring_view value(path);
    const auto separator = value.find_last_of(L"\\/");
    const auto name = separator == std::wstring_view::npos
        ? value
        : value.substr(separator + 1);
    return EqualsOrdinalIgnoreCase(name, L"DRAKS0005.sl2");
}

bool CanonicalDedicatedPath(
    const std::wstring& source,
    std::wstring& canonical) {
    if (source.empty() || source.size() >= 32768) {
        return false;
    }
    std::wstring buffer(source.size() + 2, L'\0');
    const auto length = GetFullPathNameW(
        source.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    buffer.resize(length);
    if (!EqualsOrdinalIgnoreCase(source, buffer)
        || buffer.size() < 3
        || buffer[1] != L':'
        || (buffer[2] != L'\\' && buffer[2] != L'/')) {
        return false;
    }
    const auto separator = buffer.find_last_of(L"\\/");
    const auto name = separator == std::wstring::npos
        ? std::wstring_view(buffer)
        : std::wstring_view(buffer).substr(separator + 1);
    if (!EqualsOrdinalIgnoreCase(name, L"DRAKS0005.rmm")) {
        return false;
    }

    const HANDLE handle = CreateFileW(
        buffer.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool regular = GetFileInformationByHandle(handle, &information)
        && (information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0
        && information.nNumberOfLinks == 1;
    CloseHandle(handle);
    if (!regular) {
        return false;
    }
    canonical = std::move(buffer);
    return true;
}

std::byte* CallInstruction(const SaveCallsiteRedirectTarget& target) noexcept {
    return target.address == nullptr
        ? nullptr
        : target.address + target.callOffset;
}

bool TargetIsValid(const SaveCallsiteRedirectTarget& target) noexcept {
    if (target.address == nullptr
        || target.callOffset > kSaveCallsiteFingerprintSize - 6) {
        return false;
    }
    const auto* call = CallInstruction(target);
    return target.expected[target.callOffset] == 0xff
        && target.expected[target.callOffset + 1] == 0x15
        && std::memcmp(
            target.address,
            target.expected.data(),
            target.expected.size()) == 0
        && call != nullptr;
}

void** ResolveImportSlot(const SaveCallsiteRedirectTarget& target) noexcept {
    auto* const call = CallInstruction(target);
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 2, sizeof(displacement));
    const auto next = reinterpret_cast<std::intptr_t>(call) + 6;
    return reinterpret_cast<void**>(next + displacement);
}

bool IsReadablePointerSlot(void** const slot) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (slot == nullptr
        || VirtualQuery(slot, &information, sizeof(information))
            != sizeof(information)
        || information.State != MEM_COMMIT
        || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(
        information.BaseAddress) + information.RegionSize;
    return address <= regionEnd && regionEnd - address >= sizeof(*slot);
}

bool IsExecutableAddress(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (address == nullptr
        || VirtualQuery(address, &information, sizeof(information))
            != sizeof(information)
        || information.State != MEM_COMMIT
        || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto protection = information.Protect & 0xff;
    return protection == PAGE_EXECUTE
        || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE
        || protection == PAGE_EXECUTE_WRITECOPY;
}

bool ExchangeImport(
    void** const slot,
    void* const expected,
    void* const replacement) noexcept {
    DWORD oldProtection = 0;
    if (slot == nullptr
        || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    auto* const previous = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot), replacement, expected);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(
        slot, sizeof(*slot), oldProtection, &ignored) != FALSE;
    return previous == expected && restored;
}

}  // namespace

SaveCallsiteRedirectInstallStatus InstallSaveCallsiteRedirect(
    const SaveCallsiteRedirectConfiguration& configuration) noexcept {
    try {
        std::unique_lock lock(redirectGate);
        if (installed) {
            return SaveCallsiteRedirectInstallStatus::InvalidConfiguration;
        }
        std::wstring dedicated;
        if (!CanonicalDedicatedPath(configuration.dedicatedRmm, dedicated)) {
            return SaveCallsiteRedirectInstallStatus::InvalidConfiguration;
        }
        if (!std::all_of(
                configuration.targets.begin(),
                configuration.targets.end(),
                &TargetIsValid)) {
            return SaveCallsiteRedirectInstallStatus::ProfileMismatch;
        }
        auto** const firstSlot = ResolveImportSlot(configuration.targets[0]);
        auto** const secondSlot = ResolveImportSlot(configuration.targets[1]);
        if (firstSlot != secondSlot || !IsReadablePointerSlot(firstSlot)) {
            return SaveCallsiteRedirectInstallStatus::ProfileMismatch;
        }
        void* const originalImport = *firstSlot;
        if (!IsExecutableAddress(originalImport)) {
            return SaveCallsiteRedirectInstallStatus::PatchFailed;
        }
        lifecycle = {};
        lifecycle.importSlot = firstSlot;
        lifecycle.originalImport = originalImport;
        activeState = std::make_shared<RedirectState>(RedirectState{
            std::move(dedicated),
        });
        if (!ExchangeImport(
                firstSlot,
                originalImport,
                reinterpret_cast<void*>(&DsrRedirectSaveCreateFileW))) {
            activeState.reset();
            lifecycle = {};
            return SaveCallsiteRedirectInstallStatus::PatchFailed;
        }
        installed = true;
        return SaveCallsiteRedirectInstallStatus::Success;
    }
    catch (...) {
        return SaveCallsiteRedirectInstallStatus::PatchFailed;
    }
}

SaveCallsiteRedirectCleanupStatus UninstallSaveCallsiteRedirect() noexcept {
    try {
        std::unique_lock lock(redirectGate);
        if (!installed) {
            return SaveCallsiteRedirectCleanupStatus::Success;
        }
        if (!ExchangeImport(
                lifecycle.importSlot,
                reinterpret_cast<void*>(&DsrRedirectSaveCreateFileW),
                lifecycle.originalImport)) {
            return SaveCallsiteRedirectCleanupStatus::Incomplete;
        }
        activeState.reset();
        lifecycle = {};
        installed = false;
        return SaveCallsiteRedirectCleanupStatus::Success;
    }
    catch (...) {
        return SaveCallsiteRedirectCleanupStatus::Incomplete;
    }
}

bool SaveCallsiteRedirectIsInstalled() noexcept {
    std::shared_lock lock(redirectGate);
    return installed;
}

HANDLE WINAPI RedirectSaveCreateFileW(
    const LPCWSTR fileName,
    const DWORD desiredAccess,
    const DWORD shareMode,
    const LPSECURITY_ATTRIBUTES securityAttributes,
    const DWORD creationDisposition,
    const DWORD flagsAndAttributes,
    const HANDLE templateFile) {
    try {
        std::shared_lock lock(redirectGate);
        const auto state = activeState;
        const auto* effective = state != nullptr && HasExactNormalSaveName(fileName)
            ? state->dedicatedRmm.c_str()
            : fileName;
        return CreateFileW(
            effective,
            desiredAccess,
            shareMode,
            securityAttributes,
            creationDisposition,
            flagsAndAttributes,
            templateFile);
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
}

}  // namespace DSRRandomizer::Save

extern "C" HANDLE WINAPI DsrRedirectSaveCreateFileW(
    const LPCWSTR fileName,
    const DWORD desiredAccess,
    const DWORD shareMode,
    const LPSECURITY_ATTRIBUTES securityAttributes,
    const DWORD creationDisposition,
    const DWORD flagsAndAttributes,
    const HANDLE templateFile) {
    return DSRRandomizer::Save::RedirectSaveCreateFileW(
        fileName,
        desiredAccess,
        shareMode,
        securityAttributes,
        creationDisposition,
        flagsAndAttributes,
        templateFile);
}

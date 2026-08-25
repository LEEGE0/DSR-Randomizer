#include "save/SaveHooks.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hooks/MinHookCoordinator.h"
#include "save/SavePathPolicy.h"

namespace DSRRandomizer::Save {
namespace {

using KnownFolderFunction = decltype(&SHGetKnownFolderPath);
using LegacyFolderFunction = decltype(&SHGetFolderPathW);
using CreateFileFunction = decltype(&CreateFileW);
using DeleteFileFunction = decltype(&DeleteFileW);
using MoveFileFunction = decltype(&MoveFileExW);
using ReplaceFileFunction = decltype(&ReplaceFileW);
using AttributesFunction = decltype(&GetFileAttributesExW);
using FindFirstFunction = decltype(&FindFirstFileExW);

struct HookTrampolines {
    KnownFolderFunction knownFolder = nullptr;
    LegacyFolderFunction legacyFolder = nullptr;
    CreateFileFunction createFile = nullptr;
    DeleteFileFunction deleteFile = nullptr;
    MoveFileFunction moveFile = nullptr;
    ReplaceFileFunction replaceFile = nullptr;
    AttributesFunction attributes = nullptr;
    FindFirstFunction findFirst = nullptr;
};

struct StableIdentity {
    std::wstring path;
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
};

struct HookContext {
    explicit HookContext(SaveHookConfiguration canonicalConfiguration)
        : configuration(std::move(canonicalConfiguration)),
          policy(SavePathPolicyConfiguration{
              configuration.virtualLogicalSave,
              configuration.realSaveRoot,
              configuration.dedicatedRmm}) {}

    SaveHookConfiguration configuration;
    SavePathPolicy policy;
    std::wstring physicalVirtualDocuments;
    std::wstring physicalExternalSaveRoot;
    HookTrampolines trampolines;
    std::vector<StableIdentity> stableIdentities;
    std::atomic<bool> denyOnly{false};
    std::atomic<std::uint64_t> inFlight{0};
};

struct HookLifecycle {
    HookPlatform* platform = nullptr;
    std::shared_ptr<HookContext> context;
    std::array<void*, 8> targets{};
    std::array<bool, 8> created{};
    bool initialized = false;
    bool mayBeEnabled = false;
};

std::mutex installMutex;
std::shared_mutex callbackGate;
std::atomic<std::shared_ptr<HookContext>> activeContext;
HookLifecycle lifecycle{};
std::atomic<bool> hooksInstalled{false};
std::array<std::atomic<std::uint64_t>, 4> auditCounters{};
std::atomic<Testing::BeforeOriginalApiCallback> beforeOriginalApiCallback{};
std::atomic<void*> beforeOriginalApiState{};

class CallbackLease final {
public:
    CallbackLease()
        : gate_(callbackGate),
          context_(activeContext.load(std::memory_order_acquire)) {
        if (context_ != nullptr) {
            context_->inFlight.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    ~CallbackLease() {
        if (context_ != nullptr) {
            context_->inFlight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    [[nodiscard]] const std::shared_ptr<HookContext>& Context() const noexcept {
        return context_;
    }

private:
    std::shared_lock<std::shared_mutex> gate_;
    std::shared_ptr<HookContext> context_;
};

bool EqualsOrdinalIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

bool StartsWithOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size()
        && EqualsOrdinalIgnoreCase(value.substr(0, prefix.size()), prefix);
}

bool IsBelow(
    const std::wstring_view path,
    const std::wstring_view root) noexcept {
    return EqualsOrdinalIgnoreCase(path, root)
        || (path.size() > root.size()
            && StartsWithOrdinalIgnoreCase(path, root)
            && (root.back() == L'\\' || path[root.size()] == L'\\'));
}

bool ContainsOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view needle) noexcept {
    if (needle.empty() || needle.size() > value.size()) {
        return false;
    }
    for (std::size_t index = 0; index + needle.size() <= value.size(); ++index) {
        if (EqualsOrdinalIgnoreCase(value.substr(index, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

bool IsAsciiDrivePath(const std::wstring_view path) noexcept {
    const auto drive = path.empty() ? L'\0' : path.front();
    return path.size() >= 3
        && ((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z'))
        && path[1] == L':'
        && path[2] == L'\\';
}

bool SegmentIsUnambiguous(const std::wstring_view segment) noexcept {
    if (segment.empty() || segment == L"." || segment == L".."
        || segment.back() == L'.' || segment.back() == L' ') {
        return false;
    }
    for (const auto character : segment) {
        if (character < L' ' || character == L':' || character == L'~'
            || character == L'?' || character == L'*' || character == L'"'
            || character == L'<' || character == L'>' || character == L'|') {
            return false;
        }
    }
    return true;
}

bool CanonicalizeLexical(
    const std::wstring_view input,
    std::wstring& canonical) {
    if (input.empty() || input.size() >= 32760
        || input.starts_with(L"\\\\") || input.starts_with(L"\\\\?\\")) {
        return false;
    }

    std::wstring normalized(input);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (!IsAsciiDrivePath(normalized) || normalized.back() == L'\\') {
        return normalized.size() == 3 && IsAsciiDrivePath(normalized)
            ? (canonical = normalized, true)
            : false;
    }

    std::size_t segmentStart = 3;
    while (segmentStart < normalized.size()) {
        const auto separator = normalized.find(L'\\', segmentStart);
        const auto segmentEnd = separator == std::wstring::npos
            ? normalized.size()
            : separator;
        if (!SegmentIsUnambiguous(std::wstring_view(normalized).substr(
                segmentStart,
                segmentEnd - segmentStart))) {
            return false;
        }
        if (separator == std::wstring::npos) {
            break;
        }
        segmentStart = separator + 1;
    }

    std::vector<wchar_t> buffer(normalized.size() + 2);
    const auto length = GetFullPathNameW(
        normalized.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    canonical.assign(buffer.data(), length);
    return IsAsciiDrivePath(canonical);
}

HANDLE OpenWithoutFollowingReparse(
    const std::wstring& path,
    const CreateFileFunction open) noexcept {
    if (open == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return open(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
}

bool FinalDosPath(HANDLE handle, std::wstring& result) {
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, 0);
    if (required == 0 || required >= 32768) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
    const auto length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        0);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }

    std::wstring finalPath(buffer.data(), length);
    constexpr std::wstring_view devicePrefix = L"\\\\?\\";
    if (finalPath.starts_with(devicePrefix)) {
        finalPath.erase(0, devicePrefix.size());
    }
    if (!IsAsciiDrivePath(finalPath)) {
        return false;
    }
    result = std::move(finalPath);
    return true;
}

bool HandleMatchesLexicalPath(
    const HANDLE handle,
    const std::wstring_view lexical) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)
        || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    std::wstring finalPath;
    return FinalDosPath(handle, finalPath)
        && EqualsOrdinalIgnoreCase(finalPath, lexical);
}

bool InspectExistingComponents(
    const std::wstring& lexical,
    const CreateFileFunction open) {
    if (!IsAsciiDrivePath(lexical)) {
        return false;
    }

    std::wstring current = lexical.substr(0, 3);
    std::size_t next = 3;
    while (true) {
        const HANDLE handle = OpenWithoutFollowingReparse(current, open);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        const bool safe = HandleMatchesLexicalPath(handle, current);
        CloseHandle(handle);
        if (!safe) {
            return false;
        }
        if (next >= lexical.size()) {
            return true;
        }

        const auto separator = lexical.find(L'\\', next);
        current = separator == std::wstring::npos
            ? lexical
            : lexical.substr(0, separator);
        next = separator == std::wstring::npos
            ? lexical.size()
            : separator + 1;
    }
}

enum class IdentityCaptureResult { Captured, Missing, Unsafe };

IdentityCaptureResult CaptureIdentity(
    const std::wstring& path,
    StableIdentity& identity) {
    const HANDLE handle = OpenWithoutFollowingReparse(path, &CreateFileW);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? IdentityCaptureResult::Missing
            : IdentityCaptureResult::Unsafe;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    const bool safe = HandleMatchesLexicalPath(handle, path)
        && GetFileInformationByHandle(handle, &information);
    CloseHandle(handle);
    if (!safe) {
        return IdentityCaptureResult::Unsafe;
    }
    identity = {
        path,
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
    };
    return IdentityCaptureResult::Captured;
}

bool IdentityIsStable(
    const StableIdentity& expected,
    const CreateFileFunction open) {
    const HANDLE handle = OpenWithoutFollowingReparse(expected.path, open);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool stable = HandleMatchesLexicalPath(handle, expected.path)
        && GetFileInformationByHandle(handle, &information)
        && information.dwVolumeSerialNumber == expected.volumeSerial
        && information.nFileIndexHigh == expected.fileIndexHigh
        && information.nFileIndexLow == expected.fileIndexLow;
    CloseHandle(handle);
    return stable;
}

std::wstring ParentPath(const std::wstring_view path) {
    const auto separator = path.find_last_of(L'\\');
    if (separator == std::wstring_view::npos || separator <= 2) {
        return {};
    }
    return std::wstring(path.substr(0, separator));
}

bool AddStableIdentity(
    HookContext& context,
    const std::wstring& path,
    const bool required) {
    if (path.empty()) {
        return false;
    }
    if (std::any_of(
            context.stableIdentities.begin(),
            context.stableIdentities.end(),
            [&](const StableIdentity& identity) {
                return EqualsOrdinalIgnoreCase(identity.path, path);
            })) {
        return true;
    }

    StableIdentity identity{};
    const auto result = CaptureIdentity(path, identity);
    if (result == IdentityCaptureResult::Captured) {
        context.stableIdentities.push_back(std::move(identity));
        return true;
    }
    return !required && result == IdentityCaptureResult::Missing;
}

bool AddStableIdentityAtOrAbove(
    HookContext& context,
    const std::wstring& path) {
    auto candidate = path;
    while (!candidate.empty()) {
        StableIdentity identity{};
        const auto result = CaptureIdentity(candidate, identity);
        if (result == IdentityCaptureResult::Captured) {
            context.stableIdentities.push_back(std::move(identity));
            return true;
        }
        if (result == IdentityCaptureResult::Unsafe) {
            return false;
        }
        auto parent = ParentPath(candidate);
        if (parent.empty() && candidate.size() > 3) {
            parent = candidate.substr(0, 3);
        }
        if (parent.empty() || EqualsOrdinalIgnoreCase(parent, candidate)) {
            return false;
        }
        candidate = std::move(parent);
    }
    return false;
}

bool StableIdentitiesMatch(const HookContext& context) {
    return std::all_of(
        context.stableIdentities.begin(),
        context.stableIdentities.end(),
        [&](const StableIdentity& identity) {
            return IdentityIsStable(identity, context.trampolines.createFile);
        });
}

const char* OperationName(const PathOperation operation) noexcept {
    switch (operation) {
    case PathOperation::Open: return "Open";
    case PathOperation::Read: return "Read";
    case PathOperation::Write: return "Write";
    case PathOperation::RenameSource: return "RenameSource";
    case PathOperation::RenameDestination: return "RenameDestination";
    case PathOperation::Delete: return "Delete";
    case PathOperation::Attributes: return "Attributes";
    case PathOperation::Enumeration: return "Enumeration";
    }
    return "Open";
}

const char* CategoryName(const SaveAuditCategory category) noexcept {
    switch (category) {
    case SaveAuditCategory::DedicatedRmm: return "DedicatedRmm";
    case SaveAuditCategory::DeniedNormal: return "DeniedNormal";
    case SaveAuditCategory::DeniedOverhaul: return "DeniedOverhaul";
    case SaveAuditCategory::Unrelated: return "Unrelated";
    }
    return "Unrelated";
}

void RecordAudit(
    const HookContext& context,
    const PathOperation operation,
    const SaveAuditCategory category) noexcept {
    auditCounters[static_cast<std::size_t>(category)].fetch_add(
        1,
        std::memory_order_relaxed);
    if (!context.configuration.diagnosticMode) {
        return;
    }

    char message[128]{};
    if (sprintf_s(
            message,
            "DSRRandomizer.SaveAudit %s %s\n",
            OperationName(operation),
            CategoryName(category)) > 0) {
        OutputDebugStringA(message);
    }
}

struct EvaluatedPath {
    class PinnedHandle final {
    public:
        explicit PinnedHandle(const HANDLE handle) noexcept : handle_(handle) {}
        ~PinnedHandle() {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
        }

        PinnedHandle(PinnedHandle&& other) noexcept
            : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
        PinnedHandle& operator=(PinnedHandle&& other) noexcept {
            if (this != &other) {
                if (handle_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle_);
                }
                handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
            }
            return *this;
        }

        PinnedHandle(const PinnedHandle&) = delete;
        PinnedHandle& operator=(const PinnedHandle&) = delete;

        [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

        void Close() noexcept {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    bool allowed;
    bool redirected;
    bool guarded;
    bool contained;
    std::wstring effective;
    std::vector<PinnedHandle> pins;
    bool leafPinned = false;
};

bool PinExistingParents(
    const std::wstring& lexical,
    const CreateFileFunction open,
    std::vector<EvaluatedPath::PinnedHandle>& pins) {
    if (open == nullptr || !IsAsciiDrivePath(lexical)) {
        return false;
    }
    const auto parent = ParentPath(lexical);
    if (parent.empty()) {
        return true;
    }

    std::size_t segmentEnd = parent.find(L'\\', 3);
    while (true) {
        const auto current = segmentEnd == std::wstring::npos
            ? parent
            : parent.substr(0, segmentEnd);
        const HANDLE lexicalHandle = open(
            current.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (lexicalHandle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(lexicalHandle, &information)) {
            CloseHandle(lexicalHandle);
            return false;
        }
        pins.emplace_back(lexicalHandle);
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            const HANDLE physicalHandle = open(
                current.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr);
            if (physicalHandle == INVALID_HANDLE_VALUE) {
                return false;
            }
            pins.emplace_back(physicalHandle);
        }

        if (segmentEnd == std::wstring::npos) {
            return true;
        }
        segmentEnd = parent.find(L'\\', segmentEnd + 1);
    }
}

bool ResolvePhysicalPath(
    const std::wstring& lexical,
    const CreateFileFunction open,
    std::wstring& physical,
    std::vector<EvaluatedPath::PinnedHandle>& pins,
    const bool pinLeaf,
    bool* const leafPinned = nullptr) {
    if (leafPinned != nullptr) {
        *leafPinned = false;
    }
    if (open == nullptr || !IsAsciiDrivePath(lexical)) {
        return false;
    }
    if (!PinExistingParents(lexical, open, pins)) {
        return false;
    }

    if (pinLeaf) {
        const HANDLE lexicalHandle = open(
            lexical.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (lexicalHandle != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(lexicalHandle, &information)) {
                CloseHandle(lexicalHandle);
                return false;
            }
            pins.emplace_back(lexicalHandle);
            if (leafPinned != nullptr) {
                *leafPinned = true;
            }

            HANDLE physicalHandle = lexicalHandle;
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                physicalHandle = open(
                    lexical.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
                if (physicalHandle == INVALID_HANDLE_VALUE) {
                    return false;
                }
                pins.emplace_back(physicalHandle);
            }
            return FinalDosPath(physicalHandle, physical);
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
    }

    auto existing = lexical;
    while (!existing.empty()) {
        const HANDLE handle = open(
            existing.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            std::wstring finalExisting;
            const bool resolved = FinalDosPath(handle, finalExisting);
            CloseHandle(handle);
            if (!resolved) {
                return false;
            }

            auto suffix = lexical.substr(existing.size());
            if (!suffix.empty()
                && finalExisting.ends_with(L'\\')
                && suffix.starts_with(L'\\')) {
                suffix.erase(0, 1);
            }
            return CanonicalizeLexical(finalExisting + suffix, physical);
        }

        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        auto parent = ParentPath(existing);
        if (parent.empty() && existing.size() > 3) {
            parent = existing.substr(0, 3);
        }
        if (parent.empty() || EqualsOrdinalIgnoreCase(parent, existing)) {
            return false;
        }
        existing = std::move(parent);
    }
    return false;
}

EvaluatedPath Denied(
    const HookContext& context,
    const PathOperation operation,
    const SaveAuditCategory category) {
    RecordAudit(context, operation, category);
    SetLastError(ERROR_ACCESS_DENIED);
    return {false, false, false, false, {}, {}};
}

EvaluatedPath EvaluatePath(
    const HookContext& context,
    const wchar_t* path,
    const PathOperation operation,
    const bool pinLeaf = false) {
    if (path == nullptr || context.denyOnly.load(std::memory_order_acquire)) {
        return Denied(context, operation, SaveAuditCategory::Unrelated);
    }

    constexpr std::wstring_view namedPipePrefix = L"\\\\.\\pipe\\";
    const std::wstring_view requested(path);
    if (requested.size() > namedPipePrefix.size()
        && StartsWithOrdinalIgnoreCase(requested, namedPipePrefix)) {
        RecordAudit(context, operation, SaveAuditCategory::Unrelated);
        return {true, false, false, false, std::wstring(requested), {}};
    }

    std::wstring lexical;
    if (!CanonicalizeLexical(requested, lexical)) {
        return Denied(context, operation, SaveAuditCategory::Unrelated);
    }

    const auto decision = context.policy.Evaluate(operation, lexical);
    if (decision.kind == PathDecisionKind::Deny) {
        return Denied(
            context,
            operation,
            ContainsOrdinalIgnoreCase(lexical, L".overhaul.sl2")
                ? SaveAuditCategory::DeniedOverhaul
                : SaveAuditCategory::DeniedNormal);
    }

    std::wstring physical;
    std::vector<EvaluatedPath::PinnedHandle> pins;
    if (!ResolvePhysicalPath(
            lexical,
            context.trampolines.createFile,
            physical,
            pins,
            false)) {
        return Denied(context, operation, SaveAuditCategory::Unrelated);
    }
    const auto physicalDecision = context.policy.Evaluate(operation, physical);
    if ((decision.kind == PathDecisionKind::Allow
            && physicalDecision.kind != PathDecisionKind::Allow)
        || (decision.kind == PathDecisionKind::Redirect
            && physicalDecision.kind != PathDecisionKind::Redirect)) {
        return Denied(
            context,
            operation,
            ContainsOrdinalIgnoreCase(physical, L".overhaul.sl2")
                ? SaveAuditCategory::DeniedOverhaul
                : SaveAuditCategory::DeniedNormal);
    }

    if (decision.kind == PathDecisionKind::Allow) {
        const bool lexicalBelowVirtual = IsBelow(
            lexical, context.configuration.virtualDocuments);
        const bool lexicalBelowExternal = IsBelow(
            lexical, context.configuration.externalSaveRoot);
        const bool physicalBelowVirtual = IsBelow(
            physical, context.physicalVirtualDocuments);
        const bool physicalBelowExternal = IsBelow(
            physical, context.physicalExternalSaveRoot);
        const bool guarded = lexicalBelowVirtual || lexicalBelowExternal
            || physicalBelowVirtual || physicalBelowExternal;
        if (lexicalBelowVirtual
            && (!StableIdentitiesMatch(context)
                || !InspectExistingComponents(
                    lexical,
                    context.trampolines.createFile))) {
            return Denied(context, operation, SaveAuditCategory::Unrelated);
        }
        bool leafPinned = false;
        if (pinLeaf && guarded) {
            std::wstring pinnedPhysical;
            if (!ResolvePhysicalPath(
                    lexical,
                    context.trampolines.createFile,
                    pinnedPhysical,
                    pins,
                    true,
                    &leafPinned)
                || !EqualsOrdinalIgnoreCase(pinnedPhysical, physical)) {
                return Denied(
                    context,
                    operation,
                    SaveAuditCategory::Unrelated);
            }
        }
        RecordAudit(context, operation, SaveAuditCategory::Unrelated);
        return {
            true,
            false,
            guarded,
            physicalBelowVirtual || physicalBelowExternal,
            std::wstring(requested),
            std::move(pins),
            leafPinned,
        };
    }

    if (!EqualsOrdinalIgnoreCase(
            lexical,
            context.configuration.virtualLogicalSave)
        || !StableIdentitiesMatch(context)
        || !InspectExistingComponents(
            lexical,
            context.trampolines.createFile)
        || !InspectExistingComponents(
            context.configuration.dedicatedRmm,
            context.trampolines.createFile)
        || !IsBelow(
            context.configuration.dedicatedRmm,
            context.configuration.externalSaveRoot)) {
        return Denied(context, operation, SaveAuditCategory::Unrelated);
    }

    std::wstring physicalDedicated;
    bool leafPinned = false;
    if (!ResolvePhysicalPath(
            context.configuration.dedicatedRmm,
            context.trampolines.createFile,
            physicalDedicated,
            pins,
            pinLeaf,
            &leafPinned)
        || context.policy.Evaluate(operation, physicalDedicated).kind
            != PathDecisionKind::Allow
        || !IsBelow(
            physicalDedicated,
            context.physicalExternalSaveRoot)) {
        return Denied(context, operation, SaveAuditCategory::Unrelated);
    }

    RecordAudit(context, operation, SaveAuditCategory::DedicatedRmm);
    return {
        true,
        true,
        true,
        true,
        context.configuration.dedicatedRmm,
        std::move(pins),
        leafPinned,
    };
}

bool ContainsWildcard(const std::wstring_view value) noexcept {
    return value.find_first_of(L"*?") != std::wstring_view::npos;
}

EvaluatedPath EvaluateEnumerationPath(
    const HookContext& context,
    const wchar_t* path) {
    if (path == nullptr) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }

    const std::wstring_view requested(path);
    if (!ContainsWildcard(requested)) {
        return EvaluatePath(context, path, PathOperation::Enumeration, true);
    }

    const auto separator = requested.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos
        || separator + 1 >= requested.size()) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }
    const auto directoryRequest = requested.substr(0, separator);
    const auto leafPattern = requested.substr(separator + 1);
    if (ContainsWildcard(directoryRequest)
        || leafPattern.find_first_of(L"\\/") != std::wstring_view::npos) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }

    std::wstring directoryText(directoryRequest);
    auto directory = EvaluatePath(
        context,
        directoryText.c_str(),
        PathOperation::Enumeration,
        true);
    if (!directory.allowed) {
        return directory;
    }

    std::wstring canonicalDirectory;
    const auto logicalProfile = ParentPath(
        context.configuration.virtualLogicalSave);
    if (!CanonicalizeLexical(directoryRequest, canonicalDirectory)
        || !EqualsOrdinalIgnoreCase(canonicalDirectory, logicalProfile)) {
        return {
            true,
            false,
            directory.guarded,
            directory.contained,
            std::wstring(requested),
            std::move(directory.pins),
        };
    }

    if (!EqualsOrdinalIgnoreCase(leafPattern, L"*")
        && !EqualsOrdinalIgnoreCase(leafPattern, L"DRAKS0005.*")) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }
    if (!StableIdentitiesMatch(context)
        || !InspectExistingComponents(
            context.configuration.dedicatedRmm,
            context.trampolines.createFile)
        || !IsBelow(
            context.configuration.dedicatedRmm,
            context.configuration.externalSaveRoot)) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }

    RecordAudit(
        context,
        PathOperation::Enumeration,
        SaveAuditCategory::DedicatedRmm);
    std::wstring physicalDedicated;
    if (!ResolvePhysicalPath(
            context.configuration.dedicatedRmm,
            context.trampolines.createFile,
            physicalDedicated,
            directory.pins,
            true)
        || !IsBelow(
            physicalDedicated,
            context.physicalExternalSaveRoot)) {
        return Denied(
            context,
            PathOperation::Enumeration,
            SaveAuditCategory::Unrelated);
    }
    return {
        true,
        true,
        true,
        true,
        context.configuration.dedicatedRmm,
        std::move(directory.pins),
    };
}

bool MutationOperandsAreContained(
    const std::initializer_list<const EvaluatedPath*> operands) noexcept {
    const bool guarded = std::any_of(
        operands.begin(),
        operands.end(),
        [](const EvaluatedPath* operand) {
            return operand != nullptr && operand->guarded;
        });
    return !guarded || std::all_of(
        operands.begin(),
        operands.end(),
        [](const EvaluatedPath* operand) {
            return operand == nullptr || operand->contained;
        });
}

PathOperation OpenOperation(const DWORD desiredAccess) noexcept {
    return (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA
        | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE)) != 0
        ? PathOperation::Write
        : PathOperation::Read;
}

void InvokeBeforeOriginalApiCallback() noexcept {
    const auto callback = beforeOriginalApiCallback.load(std::memory_order_acquire);
    if (callback != nullptr) {
        callback(beforeOriginalApiState.load(std::memory_order_acquire));
    }
}

HRESULT WINAPI HookKnownFolder(
    REFKNOWNFOLDERID folderId,
    const DWORD flags,
    const HANDLE token,
    PWSTR* path) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr
            || context->denyOnly.load(std::memory_order_acquire)) {
            return E_FAIL;
        }
        if (IsEqualGUID(folderId, FOLDERID_Documents)) {
            if (path == nullptr) {
                return E_INVALIDARG;
            }
            if (!StableIdentitiesMatch(*context)
                || !InspectExistingComponents(
                    context->configuration.virtualDocuments,
                    context->trampolines.createFile)) {
                return E_FAIL;
            }
            const auto characters = context->configuration.virtualDocuments.size() + 1;
            auto* allocated = static_cast<PWSTR>(
                CoTaskMemAlloc(characters * sizeof(wchar_t)));
            if (allocated == nullptr) {
                return E_OUTOFMEMORY;
            }
            std::wmemcpy(
                allocated,
                context->configuration.virtualDocuments.c_str(),
                characters);
            *path = allocated;
            return S_OK;
        }
        return context->trampolines.knownFolder == nullptr
            ? E_FAIL
            : context->trampolines.knownFolder(folderId, flags, token, path);
    }
    catch (...) {
        return E_FAIL;
    }
}

HRESULT WINAPI HookLegacyFolder(
    const HWND owner,
    const int folder,
    const HANDLE token,
    const DWORD flags,
    const LPWSTR path) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr
            || context->denyOnly.load(std::memory_order_acquire)) {
            return E_FAIL;
        }
        if ((folder & ~CSIDL_FLAG_MASK) == CSIDL_PERSONAL) {
            if (path == nullptr
                || context->configuration.virtualDocuments.size() >= MAX_PATH
                || !StableIdentitiesMatch(*context)
                || !InspectExistingComponents(
                    context->configuration.virtualDocuments,
                    context->trampolines.createFile)) {
                return E_FAIL;
            }
            std::wmemcpy(
                path,
                context->configuration.virtualDocuments.c_str(),
                context->configuration.virtualDocuments.size() + 1);
            return S_OK;
        }
        return context->trampolines.legacyFolder == nullptr
            ? E_FAIL
            : context->trampolines.legacyFolder(owner, folder, token, flags, path);
    }
    catch (...) {
        return E_FAIL;
    }
}

bool IsPrivateRegularHandle(
    const HANDLE handle,
    const std::wstring_view expectedPath,
    BY_HANDLE_FILE_INFORMATION* const observedInformation = nullptr) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)
        || (information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || information.nNumberOfLinks != 1) {
        return false;
    }
    if (observedInformation != nullptr) {
        *observedInformation = information;
    }
    std::wstring finalPath;
    return FinalDosPath(handle, finalPath)
        && EqualsOrdinalIgnoreCase(finalPath, expectedPath);
}

bool IsPrivateRegularFile(
    const std::wstring& path,
    const CreateFileFunction open) {
    const HANDLE handle = open(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool safe = IsPrivateRegularHandle(handle, path);
    CloseHandle(handle);
    return safe;
}

DWORD ReopenFlags(const DWORD flags) noexcept {
    return flags & (
        FILE_FLAG_BACKUP_SEMANTICS
        | FILE_FLAG_DELETE_ON_CLOSE
        | FILE_FLAG_NO_BUFFERING
        | FILE_FLAG_OPEN_NO_RECALL
        | FILE_FLAG_OPEN_REPARSE_POINT
        | FILE_FLAG_OVERLAPPED
        | FILE_FLAG_RANDOM_ACCESS
        | FILE_FLAG_SEQUENTIAL_SCAN
        | FILE_FLAG_WRITE_THROUGH);
}

HANDLE ReopenPinnedLeaf(
    EvaluatedPath& evaluated,
    const DWORD desiredAccess,
    const DWORD shareMode,
    const LPSECURITY_ATTRIBUTES security,
    const DWORD creation,
    const DWORD flags,
    bool& handled) {
    handled = false;
    if (!evaluated.guarded || !evaluated.leafPinned) {
        return INVALID_HANDLE_VALUE;
    }
    handled = true;
    if (evaluated.pins.empty()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }

    std::wstring expected;
    if (!CanonicalizeLexical(evaluated.effective, expected)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    auto& strongPin = evaluated.pins.back();
    BY_HANDLE_FILE_INFORMATION existingInformation{};
    if (!IsPrivateRegularHandle(
            strongPin.Get(),
            expected,
            &existingInformation)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (creation == CREATE_NEW) {
        SetLastError(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    if (creation != OPEN_EXISTING
        && creation != OPEN_ALWAYS
        && creation != CREATE_ALWAYS
        && creation != TRUNCATE_EXISTING) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (creation == CREATE_ALWAYS) {
        constexpr DWORD protectedAttributes =
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
        const DWORD requiredAttributes =
            existingInformation.dwFileAttributes & protectedAttributes;
        const DWORD requestedAttributes = flags & protectedAttributes;
        if ((requiredAttributes & ~requestedAttributes) != 0) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
    }

    const HANDLE weakPin = ReOpenFile(
        strongPin.Get(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0);
    if (weakPin == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    strongPin.Close();

    const HANDLE result = ReOpenFile(
        weakPin,
        desiredAccess,
        shareMode,
        ReopenFlags(flags));
    const auto reopenError = GetLastError();
    CloseHandle(weakPin);
    if (result == INVALID_HANDLE_VALUE) {
        SetLastError(reopenError);
        return INVALID_HANDLE_VALUE;
    }
    if (!IsPrivateRegularHandle(result, expected)) {
        CloseHandle(result);
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (security != nullptr && security->bInheritHandle
        && !SetHandleInformation(
            result,
            HANDLE_FLAG_INHERIT,
            HANDLE_FLAG_INHERIT)) {
        const auto inheritError = GetLastError();
        CloseHandle(result);
        SetLastError(inheritError);
        return INVALID_HANDLE_VALUE;
    }

    if (creation == CREATE_ALWAYS || creation == TRUNCATE_EXISTING) {
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(result, beginning, nullptr, FILE_BEGIN)
            || !SetEndOfFile(result)) {
            const auto truncateError = GetLastError();
            CloseHandle(result);
            SetLastError(truncateError);
            return INVALID_HANDLE_VALUE;
        }
    }
    SetLastError(
        creation == CREATE_ALWAYS || creation == OPEN_ALWAYS
            ? ERROR_ALREADY_EXISTS
            : ERROR_SUCCESS);
    return result;
}

HANDLE WINAPI HookCreateFile(
    const LPCWSTR fileName,
    const DWORD desiredAccess,
    const DWORD shareMode,
    const LPSECURITY_ATTRIBUTES security,
    const DWORD creation,
    const DWORD flags,
    const HANDLE templateFile) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.createFile == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        auto evaluated = EvaluatePath(
            *context,
            fileName,
            OpenOperation(desiredAccess),
            true);
        if (!evaluated.allowed) {
            return INVALID_HANDLE_VALUE;
        }
        InvokeBeforeOriginalApiCallback();
        bool reopenedPinnedLeaf = false;
        const HANDLE pinnedResult = ReopenPinnedLeaf(
            evaluated,
            desiredAccess,
            shareMode,
            security,
            creation,
            flags,
            reopenedPinnedLeaf);
        if (reopenedPinnedLeaf) {
            return pinnedResult;
        }
        return context->trampolines.createFile(
            evaluated.effective.c_str(),
            desiredAccess,
            shareMode,
            security,
            creation,
            flags,
            templateFile);
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
}

BOOL WINAPI HookDeleteFile(const LPCWSTR fileName) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.deleteFile == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const auto evaluated = EvaluatePath(
            *context,
            fileName,
            PathOperation::Delete);
        return evaluated.allowed
            ? context->trampolines.deleteFile(evaluated.effective.c_str())
            : FALSE;
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

BOOL WINAPI HookMoveFile(
    const LPCWSTR existingName,
    const LPCWSTR newName,
    const DWORD flags) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.moveFile == nullptr
            || newName == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const auto source = EvaluatePath(
            *context,
            existingName,
            PathOperation::RenameSource);
        if (!source.allowed) {
            return FALSE;
        }
        const auto destination = EvaluatePath(
            *context,
            newName,
            PathOperation::RenameDestination);
        return destination.allowed
                && MutationOperandsAreContained({&source, &destination})
            ? context->trampolines.moveFile(
                source.effective.c_str(),
                destination.effective.c_str(),
                flags)
            : (SetLastError(ERROR_ACCESS_DENIED), FALSE);
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

BOOL WINAPI HookReplaceFile(
    const LPCWSTR replacedName,
    const LPCWSTR replacementName,
    const LPCWSTR backupName,
    const DWORD flags,
    const LPVOID exclude,
    const LPVOID reserved) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.replaceFile == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const auto replaced = EvaluatePath(
            *context,
            replacedName,
            PathOperation::RenameDestination);
        if (!replaced.allowed) {
            return FALSE;
        }
        const auto replacement = EvaluatePath(
            *context,
            replacementName,
            PathOperation::RenameSource);
        if (!replacement.allowed) {
            return FALSE;
        }

        std::wstring backup;
        const wchar_t* effectiveBackup = nullptr;
        EvaluatedPath evaluatedBackup{true, false, false, false, {}, {}};
        if (backupName != nullptr) {
            evaluatedBackup = EvaluatePath(
                *context,
                backupName,
                PathOperation::RenameDestination);
            if (!evaluatedBackup.allowed) {
                return FALSE;
            }
            backup = evaluatedBackup.effective;
            effectiveBackup = backup.c_str();
        }
        if (!MutationOperandsAreContained({
                &replaced,
                &replacement,
                backupName == nullptr ? nullptr : &evaluatedBackup})) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        return context->trampolines.replaceFile(
            replaced.effective.c_str(),
            replacement.effective.c_str(),
            effectiveBackup,
            flags,
            exclude,
            reserved);
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

BOOL WINAPI HookAttributes(
    const LPCWSTR fileName,
    const GET_FILEEX_INFO_LEVELS infoLevel,
    const LPVOID information) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.attributes == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const auto evaluated = EvaluatePath(
            *context,
            fileName,
            PathOperation::Attributes,
            true);
        return evaluated.allowed
            ? context->trampolines.attributes(
                evaluated.effective.c_str(),
                infoLevel,
                information)
            : FALSE;
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

HANDLE WINAPI HookFindFirst(
    const LPCWSTR fileName,
    const FINDEX_INFO_LEVELS infoLevel,
    const LPVOID findData,
    const FINDEX_SEARCH_OPS searchOperation,
    const LPVOID searchFilter,
    const DWORD flags) {
    try {
        CallbackLease callback;
        const auto& context = callback.Context();
        if (context == nullptr || context->trampolines.findFirst == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        const auto evaluated = EvaluateEnumerationPath(
            *context,
            fileName);
        if (!evaluated.allowed) {
            return INVALID_HANDLE_VALUE;
        }
        const auto result = context->trampolines.findFirst(
            evaluated.effective.c_str(),
            infoLevel,
            findData,
            searchOperation,
            searchFilter,
            flags);
        if (result != INVALID_HANDLE_VALUE && evaluated.redirected && findData != nullptr) {
            auto* data = static_cast<WIN32_FIND_DATAW*>(findData);
            if (wcscpy_s(data->cFileName, MAX_PATH, L"DRAKS0005.sl2") != 0) {
                FindClose(result);
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return INVALID_HANDLE_VALUE;
            }
            data->cAlternateFileName[0] = L'\0';
        }
        return result;
    }
    catch (...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
}

struct HookDefinition {
    const wchar_t* module;
    const char* procedure;
    void* detour;
    void** original;
};

std::array<HookDefinition, 8> HookDefinitions(HookContext& context) {
    return {{
        {L"shell32.dll", "SHGetKnownFolderPath",
         reinterpret_cast<void*>(&HookKnownFolder),
         reinterpret_cast<void**>(&context.trampolines.knownFolder)},
        {L"shell32.dll", "SHGetFolderPathW",
         reinterpret_cast<void*>(&HookLegacyFolder),
         reinterpret_cast<void**>(&context.trampolines.legacyFolder)},
        {L"kernel32.dll", "CreateFileW",
         reinterpret_cast<void*>(&HookCreateFile),
         reinterpret_cast<void**>(&context.trampolines.createFile)},
        {L"kernel32.dll", "DeleteFileW",
         reinterpret_cast<void*>(&HookDeleteFile),
         reinterpret_cast<void**>(&context.trampolines.deleteFile)},
        {L"kernel32.dll", "MoveFileExW",
         reinterpret_cast<void*>(&HookMoveFile),
         reinterpret_cast<void**>(&context.trampolines.moveFile)},
        {L"kernel32.dll", "ReplaceFileW",
         reinterpret_cast<void*>(&HookReplaceFile),
         reinterpret_cast<void**>(&context.trampolines.replaceFile)},
        {L"kernel32.dll", "GetFileAttributesExW",
         reinterpret_cast<void*>(&HookAttributes),
         reinterpret_cast<void**>(&context.trampolines.attributes)},
        {L"kernel32.dll", "FindFirstFileExW",
         reinterpret_cast<void*>(&HookFindFirst),
         reinterpret_cast<void**>(&context.trampolines.findFirst)},
    }};
}

class MinHookPlatform final : public HookPlatform {
public:
    void BeginMutation() noexcept override {
        if (mutationDepth_++ == 0) {
            try {
                mutationLease_ = std::make_unique<Hooks::MinHookMutationLease>();
            }
            catch (...) {
                std::terminate();
            }
        }
    }

    void EndMutation() noexcept override {
        if (mutationDepth_ != 0 && --mutationDepth_ == 0) {
            mutationLease_.reset();
        }
    }

    bool Initialize() noexcept override {
        targetCount_ = 0;
        initialized_ = Hooks::AcquireMinHook();
        return initialized_;
    }

    void* ResolveTarget(
        const wchar_t* moduleName,
        const char* procedureName) noexcept override {
        auto module = GetModuleHandleW(moduleName);
        if (module == nullptr) {
            module = LoadLibraryW(moduleName);
        }
        return module == nullptr
            ? nullptr
            : reinterpret_cast<void*>(GetProcAddress(module, procedureName));
    }

    bool CreateHook(
        void* target,
        void* detour,
        void** original) noexcept override {
        if (targetCount_ >= targets_.size()
            || Hooks::CreateHook(target, detour, original) != MH_OK) {
            return false;
        }
        targets_[targetCount_++] = target;
        return true;
    }

    bool QueueEnable(void* target) noexcept override {
        return Hooks::QueueEnableHook(target) == MH_OK;
    }

    bool ApplyQueued() noexcept override {
        return Hooks::ApplyQueuedHooks() == MH_OK;
    }

    bool DisableAll() noexcept override {
        bool disabled = true;
        for (std::size_t index = 0; index < targetCount_; ++index) {
            const auto status = Hooks::DisableHook(targets_[index]);
            disabled = (status == MH_OK
                    || status == MH_ERROR_DISABLED
                    || status == MH_ERROR_NOT_CREATED)
                && disabled;
        }
        return disabled;
    }

    bool RemoveHook(void* target) noexcept override {
        const auto status = Hooks::RemoveHook(target);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
            return false;
        }
        const auto found = std::find(
            targets_.begin(),
            targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
            target);
        if (found != targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_)) {
            std::move(
                found + 1,
                targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
                found);
            --targetCount_;
        }
        return true;
    }

    bool Uninitialize() noexcept override {
        if (!initialized_) {
            return targetCount_ == 0;
        }
        if (!Hooks::ReleaseMinHook()) {
            return false;
        }
        initialized_ = false;
        targetCount_ = 0;
        return true;
    }

private:
    bool initialized_ = false;
    std::array<void*, 8> targets_{};
    std::size_t targetCount_ = 0;
    std::size_t mutationDepth_ = 0;
    std::unique_ptr<Hooks::MinHookMutationLease> mutationLease_;
};

MinHookPlatform systemPlatform;

class HookPlatformMutation final {
public:
    explicit HookPlatformMutation(HookPlatform* const platform) noexcept
        : platform_(platform) {
        if (platform_ != nullptr) {
            platform_->BeginMutation();
        }
    }
    ~HookPlatformMutation() {
        if (platform_ != nullptr) {
            platform_->EndMutation();
        }
    }

private:
    HookPlatform* platform_;
};

bool ValidateConfiguration(
    const SaveHookConfiguration& configuration,
    std::shared_ptr<HookContext>& context) {
    if (configuration.virtualDocuments.size() >= MAX_PATH) {
        return false;
    }

    SaveHookConfiguration canonical{};
    canonical.diagnosticMode = configuration.diagnosticMode;
    if (!CanonicalizeLexical(configuration.virtualDocuments, canonical.virtualDocuments)
        || !CanonicalizeLexical(configuration.virtualLogicalSave, canonical.virtualLogicalSave)
        || !CanonicalizeLexical(configuration.realSaveRoot, canonical.realSaveRoot)
        || !CanonicalizeLexical(configuration.externalSaveRoot, canonical.externalSaveRoot)
        || !CanonicalizeLexical(configuration.dedicatedRmm, canonical.dedicatedRmm)) {
        return false;
    }

    if (!EqualsOrdinalIgnoreCase(configuration.virtualDocuments, canonical.virtualDocuments)
        || !EqualsOrdinalIgnoreCase(configuration.virtualLogicalSave, canonical.virtualLogicalSave)
        || !EqualsOrdinalIgnoreCase(configuration.realSaveRoot, canonical.realSaveRoot)
        || !EqualsOrdinalIgnoreCase(configuration.externalSaveRoot, canonical.externalSaveRoot)
        || !EqualsOrdinalIgnoreCase(configuration.dedicatedRmm, canonical.dedicatedRmm)
        || !IsBelow(canonical.virtualLogicalSave, canonical.virtualDocuments)
        || !IsBelow(canonical.dedicatedRmm, canonical.externalSaveRoot)
        || !InspectExistingComponents(canonical.virtualDocuments, &CreateFileW)
        || !InspectExistingComponents(canonical.virtualLogicalSave, &CreateFileW)
        || !InspectExistingComponents(canonical.realSaveRoot, &CreateFileW)
        || !InspectExistingComponents(canonical.externalSaveRoot, &CreateFileW)
        || !InspectExistingComponents(canonical.dedicatedRmm, &CreateFileW)
        || !IsPrivateRegularFile(canonical.dedicatedRmm, &CreateFileW)) {
        return false;
    }

    auto candidate = std::make_shared<HookContext>(std::move(canonical));
    std::vector<EvaluatedPath::PinnedHandle> validationPins;
    if (!ResolvePhysicalPath(
            candidate->configuration.virtualDocuments,
            &CreateFileW,
            candidate->physicalVirtualDocuments,
            validationPins,
            false)
        || !ResolvePhysicalPath(
            candidate->configuration.externalSaveRoot,
            &CreateFileW,
            candidate->physicalExternalSaveRoot,
            validationPins,
            false)) {
        return false;
    }
    const auto logical = candidate->policy.Evaluate(
        PathOperation::Open,
        candidate->configuration.virtualLogicalSave);
    const auto normalRoot = candidate->policy.Evaluate(
        PathOperation::Open,
        candidate->configuration.realSaveRoot);
    if (logical.kind != PathDecisionKind::Redirect
        || !EqualsOrdinalIgnoreCase(
            logical.EffectivePath(),
            candidate->configuration.dedicatedRmm)
        || normalRoot.kind != PathDecisionKind::Deny
        || !AddStableIdentity(*candidate, candidate->configuration.virtualDocuments, true)
        || !AddStableIdentity(
            *candidate,
            ParentPath(candidate->configuration.virtualLogicalSave),
            true)
        || !AddStableIdentityAtOrAbove(
            *candidate,
            candidate->configuration.realSaveRoot)
        || !AddStableIdentity(*candidate, candidate->configuration.externalSaveRoot, true)) {
        return false;
    }

    context = std::move(candidate);
    return true;
}

SaveHookCleanupStatus CleanupLocked() noexcept {
    HookPlatformMutation mutation(lifecycle.platform);
    hooksInstalled.store(false, std::memory_order_release);
    if (lifecycle.context != nullptr) {
        lifecycle.context->denyOnly.store(true, std::memory_order_release);
    }
    if (lifecycle.platform == nullptr) {
        std::unique_lock callbackLock(callbackGate);
        activeContext.store({}, std::memory_order_release);
        lifecycle = {};
        return SaveHookCleanupStatus::Success;
    }

    if (lifecycle.mayBeEnabled) {
        if (!lifecycle.platform->DisableAll()) {
            return SaveHookCleanupStatus::Incomplete;
        }
        lifecycle.mayBeEnabled = false;
    }

    std::unique_lock callbackLock(callbackGate);
    bool allRemoved = true;
    for (std::size_t index = lifecycle.created.size(); index > 0; --index) {
        const auto slot = index - 1;
        if (!lifecycle.created[slot]) {
            continue;
        }
        if (lifecycle.platform->RemoveHook(lifecycle.targets[slot])) {
            lifecycle.created[slot] = false;
        }
        else {
            allRemoved = false;
        }
    }
    if (!allRemoved) {
        return SaveHookCleanupStatus::Incomplete;
    }

    if (lifecycle.initialized) {
        if (!lifecycle.platform->Uninitialize()) {
            return SaveHookCleanupStatus::Incomplete;
        }
        lifecycle.initialized = false;
    }

    activeContext.store({}, std::memory_order_release);
    lifecycle = {};
    return SaveHookCleanupStatus::Success;
}

}  // namespace

SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration) noexcept {
    return InstallSaveHooks(configuration, systemPlatform);
}

SaveHookInstallStatus InstallSaveHooks(
    const SaveHookConfiguration& configuration,
    HookPlatform& platform) noexcept {
    std::scoped_lock lock(installMutex);
    if (lifecycle.platform != nullptr
        || activeContext.load(std::memory_order_acquire) != nullptr) {
        return SaveHookInstallStatus::InstallFailed;
    }

    HookPlatformMutation mutation(&platform);

    try {
        std::shared_ptr<HookContext> context;
        if (!ValidateConfiguration(configuration, context)) {
            return SaveHookInstallStatus::InvalidConfiguration;
        }
        if (!platform.Initialize()) {
            return SaveHookInstallStatus::InstallFailed;
        }

        lifecycle.platform = &platform;
        lifecycle.context = context;
        lifecycle.initialized = true;
        activeContext.store(context, std::memory_order_release);

        const auto definitions = HookDefinitions(*context);
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            lifecycle.targets[index] = platform.ResolveTarget(
                definitions[index].module,
                definitions[index].procedure);
            if (lifecycle.targets[index] == nullptr) {
                static_cast<void>(CleanupLocked());
                return SaveHookInstallStatus::InstallFailed;
            }
        }

        for (std::size_t index = 0; index < definitions.size(); ++index) {
            if (!platform.CreateHook(
                    lifecycle.targets[index],
                    definitions[index].detour,
                    definitions[index].original)) {
                static_cast<void>(CleanupLocked());
                return SaveHookInstallStatus::InstallFailed;
            }
            lifecycle.created[index] = true;
        }

        for (const auto target : lifecycle.targets) {
            if (!platform.QueueEnable(target)) {
                static_cast<void>(CleanupLocked());
                return SaveHookInstallStatus::InstallFailed;
            }
        }
        lifecycle.mayBeEnabled = true;
        if (!platform.ApplyQueued()) {
            static_cast<void>(CleanupLocked());
            return SaveHookInstallStatus::InstallFailed;
        }

        hooksInstalled.store(true, std::memory_order_release);
        return SaveHookInstallStatus::Success;
    }
    catch (...) {
        static_cast<void>(CleanupLocked());
        return SaveHookInstallStatus::InstallFailed;
    }
}

SaveHookCleanupStatus UninstallSaveHooks() noexcept {
    std::scoped_lock lock(installMutex);
    return CleanupLocked();
}

bool SaveHooksAreInstalled() noexcept {
    return hooksInstalled.load(std::memory_order_acquire);
}

SaveAuditCounters CurrentSaveAuditCounters() noexcept {
    return {
        auditCounters[static_cast<std::size_t>(SaveAuditCategory::DedicatedRmm)]
            .load(std::memory_order_relaxed),
        auditCounters[static_cast<std::size_t>(SaveAuditCategory::DeniedNormal)]
            .load(std::memory_order_relaxed),
        auditCounters[static_cast<std::size_t>(SaveAuditCategory::DeniedOverhaul)]
            .load(std::memory_order_relaxed),
        auditCounters[static_cast<std::size_t>(SaveAuditCategory::Unrelated)]
            .load(std::memory_order_relaxed),
    };
}

namespace Testing {

SaveHookLifecycleSnapshot CurrentSaveHookLifecycle() noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    return {
        hooksInstalled.load(std::memory_order_acquire),
        context != nullptr,
        context != nullptr
            && context->denyOnly.load(std::memory_order_acquire),
        context == nullptr
            ? 0
            : context->inFlight.load(std::memory_order_acquire),
    };
}

void SetBeforeOriginalApiCallback(
    const BeforeOriginalApiCallback callback,
    void* state) noexcept {
    beforeOriginalApiState.store(state, std::memory_order_release);
    beforeOriginalApiCallback.store(callback, std::memory_order_release);
}

void HoldSaveHookCallback(void* enteredEvent, void* releaseEvent) noexcept {
    CallbackLease callback;
    if (enteredEvent != nullptr) {
        SetEvent(static_cast<HANDLE>(enteredEvent));
    }
    if (releaseEvent != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(releaseEvent), INFINITE);
    }
}

}  // namespace Testing

}  // namespace DSRRandomizer::Save

#include "save/SaveHooks.h"

#include <Windows.h>
#include <ShlObj.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

KnownFolderFunction originalKnownFolder = nullptr;
LegacyFolderFunction originalLegacyFolder = nullptr;
CreateFileFunction originalCreateFile = nullptr;
DeleteFileFunction originalDeleteFile = nullptr;
MoveFileFunction originalMoveFile = nullptr;
ReplaceFileFunction originalReplaceFile = nullptr;
AttributesFunction originalAttributes = nullptr;
FindFirstFunction originalFindFirst = nullptr;

std::mutex installMutex;
std::unique_ptr<SavePathPolicy> activePolicy;
SaveHookConfiguration activeConfiguration{};
std::array<void*, 8> installedTargets{};
std::size_t installedTargetCount = 0;
HookPlatform* installedPlatform = nullptr;
std::atomic<bool> hooksInstalled{false};
std::array<std::atomic<std::uint64_t>, 4> auditCounters{};

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

HANDLE OpenForResolution(const std::wstring& path, const bool useOriginal) {
    const auto open = useOriginal && originalCreateFile != nullptr
        ? originalCreateFile
        : &CreateFileW;
    return open(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
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

bool ResolvePhysicalDosPath(
    const std::wstring_view input,
    const bool useOriginal,
    std::wstring& resolved) {
    std::wstring lexical;
    if (!CanonicalizeLexical(input, lexical)) {
        return false;
    }

    std::vector<std::wstring> missingSegments;
    std::wstring probe = lexical;
    HANDLE handle = INVALID_HANDLE_VALUE;
    while (true) {
        handle = OpenForResolution(probe, useOriginal);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }

        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        const auto separator = probe.find_last_of(L'\\');
        if (separator == std::wstring::npos || separator < 2 || probe.size() == 3) {
            return false;
        }
        const auto segment = probe.substr(separator + 1);
        if (!SegmentIsUnambiguous(segment)) {
            return false;
        }
        missingSegments.push_back(segment);
        probe.erase(separator == 2 ? 3 : separator);
    }

    std::wstring physical;
    const bool finalPathSucceeded = FinalDosPath(handle, physical);
    CloseHandle(handle);
    if (!finalPathSucceeded) {
        return false;
    }

    for (auto segment = missingSegments.rbegin();
         segment != missingSegments.rend();
         ++segment) {
        if (physical.back() != L'\\') {
            physical.push_back(L'\\');
        }
        physical.append(*segment);
    }

    std::wstring checked;
    if (!CanonicalizeLexical(physical, checked)) {
        return false;
    }
    resolved = std::move(checked);
    return true;
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
    const PathOperation operation,
    const SaveAuditCategory category) noexcept {
    auditCounters[static_cast<std::size_t>(category)].fetch_add(
        1,
        std::memory_order_relaxed);
    if (!activeConfiguration.diagnosticMode) {
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
    bool allowed;
    bool redirected;
    std::wstring effective;
};

EvaluatedPath Denied(const PathOperation operation, const SaveAuditCategory category) {
    RecordAudit(operation, category);
    SetLastError(ERROR_ACCESS_DENIED);
    return {false, false, {}};
}

EvaluatedPath EvaluatePath(const wchar_t* path, const PathOperation operation) {
    if (path == nullptr || activePolicy == nullptr) {
        return Denied(operation, SaveAuditCategory::Unrelated);
    }

    constexpr std::wstring_view namedPipePrefix = L"\\\\.\\pipe\\";
    const std::wstring_view requested(path);
    if (requested.size() > namedPipePrefix.size()
        && StartsWithOrdinalIgnoreCase(requested, namedPipePrefix)) {
        RecordAudit(operation, SaveAuditCategory::Unrelated);
        return {true, false, std::wstring(requested)};
    }

    std::wstring canonical;
    if (!ResolvePhysicalDosPath(requested, true, canonical)) {
        return Denied(operation, SaveAuditCategory::Unrelated);
    }

    const auto decision = activePolicy->Evaluate(operation, canonical);
    if (decision.kind == PathDecisionKind::Deny) {
        return Denied(
            operation,
            ContainsOrdinalIgnoreCase(canonical, L".overhaul.sl2")
                ? SaveAuditCategory::DeniedOverhaul
                : SaveAuditCategory::DeniedNormal);
    }
    if (decision.kind == PathDecisionKind::Allow) {
        RecordAudit(operation, SaveAuditCategory::Unrelated);
        return {true, false, std::move(canonical)};
    }

    std::wstring currentExternalRoot;
    std::wstring currentDedicated;
    if (!ResolvePhysicalDosPath(
            activeConfiguration.externalSaveRoot,
            true,
            currentExternalRoot)
        || !ResolvePhysicalDosPath(
            decision.EffectivePath(),
            true,
            currentDedicated)
        || !EqualsOrdinalIgnoreCase(
            currentExternalRoot,
            activeConfiguration.externalSaveRoot)
        || !EqualsOrdinalIgnoreCase(
            currentDedicated,
            activeConfiguration.dedicatedRmm)
        || !IsBelow(currentDedicated, currentExternalRoot)) {
        return Denied(operation, SaveAuditCategory::Unrelated);
    }

    RecordAudit(operation, SaveAuditCategory::DedicatedRmm);
    return {true, true, std::move(currentDedicated)};
}

PathOperation OpenOperation(const DWORD desiredAccess) noexcept {
    return (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA
        | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE)) != 0
        ? PathOperation::Write
        : PathOperation::Read;
}

HRESULT WINAPI HookKnownFolder(
    REFKNOWNFOLDERID folderId,
    const DWORD flags,
    const HANDLE token,
    PWSTR* path) {
    if (IsEqualGUID(folderId, FOLDERID_Documents)) {
        if (path == nullptr) {
            return E_INVALIDARG;
        }
        const auto characters = activeConfiguration.virtualDocuments.size() + 1;
        auto* allocated = static_cast<PWSTR>(CoTaskMemAlloc(characters * sizeof(wchar_t)));
        if (allocated == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::wmemcpy(
            allocated,
            activeConfiguration.virtualDocuments.c_str(),
            characters);
        *path = allocated;
        return S_OK;
    }
    return originalKnownFolder(folderId, flags, token, path);
}

HRESULT WINAPI HookLegacyFolder(
    const HWND owner,
    const int folder,
    const HANDLE token,
    const DWORD flags,
    const LPWSTR path) {
    if ((folder & ~CSIDL_FLAG_MASK) == CSIDL_PERSONAL) {
        if (path == nullptr
            || activeConfiguration.virtualDocuments.size() >= MAX_PATH) {
            return E_FAIL;
        }
        std::wmemcpy(
            path,
            activeConfiguration.virtualDocuments.c_str(),
            activeConfiguration.virtualDocuments.size() + 1);
        return S_OK;
    }
    return originalLegacyFolder(owner, folder, token, flags, path);
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
        const auto evaluated = EvaluatePath(fileName, OpenOperation(desiredAccess));
        if (!evaluated.allowed) {
            return INVALID_HANDLE_VALUE;
        }
        return originalCreateFile(
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
        const auto evaluated = EvaluatePath(fileName, PathOperation::Delete);
        return evaluated.allowed
            ? originalDeleteFile(evaluated.effective.c_str())
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
        if (newName == nullptr) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const auto source = EvaluatePath(existingName, PathOperation::RenameSource);
        if (!source.allowed) {
            return FALSE;
        }
        const auto destination = EvaluatePath(newName, PathOperation::RenameDestination);
        return destination.allowed
            ? originalMoveFile(
                source.effective.c_str(),
                destination.effective.c_str(),
                flags)
            : FALSE;
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
        const auto replaced = EvaluatePath(
            replacedName,
            PathOperation::RenameDestination);
        if (!replaced.allowed) {
            return FALSE;
        }
        const auto replacement = EvaluatePath(
            replacementName,
            PathOperation::RenameSource);
        if (!replacement.allowed) {
            return FALSE;
        }

        std::wstring backup;
        const wchar_t* effectiveBackup = nullptr;
        if (backupName != nullptr) {
            const auto evaluatedBackup = EvaluatePath(
                backupName,
                PathOperation::RenameDestination);
            if (!evaluatedBackup.allowed) {
                return FALSE;
            }
            backup = evaluatedBackup.effective;
            effectiveBackup = backup.c_str();
        }
        return originalReplaceFile(
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
        const auto evaluated = EvaluatePath(fileName, PathOperation::Attributes);
        return evaluated.allowed
            ? originalAttributes(evaluated.effective.c_str(), infoLevel, information)
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
        const auto evaluated = EvaluatePath(fileName, PathOperation::Enumeration);
        if (!evaluated.allowed) {
            return INVALID_HANDLE_VALUE;
        }
        const auto result = originalFindFirst(
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

std::array<HookDefinition, 8> HookDefinitions() {
    return {{
        {L"shell32.dll", "SHGetKnownFolderPath",
         reinterpret_cast<void*>(&HookKnownFolder),
         reinterpret_cast<void**>(&originalKnownFolder)},
        {L"shell32.dll", "SHGetFolderPathW",
         reinterpret_cast<void*>(&HookLegacyFolder),
         reinterpret_cast<void**>(&originalLegacyFolder)},
        {L"kernel32.dll", "CreateFileW",
         reinterpret_cast<void*>(&HookCreateFile),
         reinterpret_cast<void**>(&originalCreateFile)},
        {L"kernel32.dll", "DeleteFileW",
         reinterpret_cast<void*>(&HookDeleteFile),
         reinterpret_cast<void**>(&originalDeleteFile)},
        {L"kernel32.dll", "MoveFileExW",
         reinterpret_cast<void*>(&HookMoveFile),
         reinterpret_cast<void**>(&originalMoveFile)},
        {L"kernel32.dll", "ReplaceFileW",
         reinterpret_cast<void*>(&HookReplaceFile),
         reinterpret_cast<void**>(&originalReplaceFile)},
        {L"kernel32.dll", "GetFileAttributesExW",
         reinterpret_cast<void*>(&HookAttributes),
         reinterpret_cast<void**>(&originalAttributes)},
        {L"kernel32.dll", "FindFirstFileExW",
         reinterpret_cast<void*>(&HookFindFirst),
         reinterpret_cast<void**>(&originalFindFirst)},
    }};
}

void ResetOriginals() noexcept {
    originalKnownFolder = nullptr;
    originalLegacyFolder = nullptr;
    originalCreateFile = nullptr;
    originalDeleteFile = nullptr;
    originalMoveFile = nullptr;
    originalReplaceFile = nullptr;
    originalAttributes = nullptr;
    originalFindFirst = nullptr;
}

class MinHookPlatform final : public HookPlatform {
public:
    bool Initialize() noexcept override {
        targetCount_ = 0;
        const auto status = MH_Initialize();
        ownsInitialization_ = status == MH_OK;
        return ownsInitialization_ || status == MH_ERROR_ALREADY_INITIALIZED;
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
        if (MH_CreateHook(target, detour, original) != MH_OK) {
            return false;
        }
        if (targetCount_ >= targets_.size()) {
            const auto status = MH_RemoveHook(target);
            static_cast<void>(status);
            return false;
        }
        targets_[targetCount_++] = target;
        return true;
    }

    bool QueueEnable(void* target) noexcept override {
        return MH_QueueEnableHook(target) == MH_OK;
    }

    bool ApplyQueued() noexcept override { return MH_ApplyQueued() == MH_OK; }

    void DisableAll() noexcept override {
        for (std::size_t index = 0; index < targetCount_; ++index) {
            const auto target = targets_[index];
            const auto status = MH_DisableHook(target);
            static_cast<void>(status);
        }
    }

    void RemoveHook(void* target) noexcept override {
        const auto status = MH_RemoveHook(target);
        static_cast<void>(status);
        const auto found = std::find(
            targets_.begin(),
            targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
            target);
        if (found != targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_)) {
            std::move(found + 1,
                targets_.begin() + static_cast<std::ptrdiff_t>(targetCount_),
                found);
            --targetCount_;
        }
    }

    void Uninitialize() noexcept override {
        if (ownsInitialization_) {
            const auto status = MH_Uninitialize();
            static_cast<void>(status);
        }
        ownsInitialization_ = false;
        targetCount_ = 0;
    }

private:
    bool ownsInitialization_ = false;
    std::array<void*, 8> targets_{};
    std::size_t targetCount_ = 0;
};

MinHookPlatform systemPlatform;

bool ValidateConfiguration(
    const SaveHookConfiguration& configuration,
    SaveHookConfiguration& canonicalConfiguration) {
    if (configuration.virtualDocuments.size() >= MAX_PATH) {
        return false;
    }

    canonicalConfiguration.diagnosticMode = configuration.diagnosticMode;
    if (!ResolvePhysicalDosPath(
            configuration.virtualDocuments,
            false,
            canonicalConfiguration.virtualDocuments)
        || !ResolvePhysicalDosPath(
            configuration.virtualLogicalSave,
            false,
            canonicalConfiguration.virtualLogicalSave)
        || !ResolvePhysicalDosPath(
            configuration.realSaveRoot,
            false,
            canonicalConfiguration.realSaveRoot)
        || !ResolvePhysicalDosPath(
            configuration.externalSaveRoot,
            false,
            canonicalConfiguration.externalSaveRoot)
        || !ResolvePhysicalDosPath(
            configuration.dedicatedRmm,
            false,
            canonicalConfiguration.dedicatedRmm)) {
        return false;
    }

    if (!EqualsOrdinalIgnoreCase(
            configuration.virtualDocuments,
            canonicalConfiguration.virtualDocuments)
        || !EqualsOrdinalIgnoreCase(
            configuration.virtualLogicalSave,
            canonicalConfiguration.virtualLogicalSave)
        || !EqualsOrdinalIgnoreCase(
            configuration.realSaveRoot,
            canonicalConfiguration.realSaveRoot)
        || !EqualsOrdinalIgnoreCase(
            configuration.externalSaveRoot,
            canonicalConfiguration.externalSaveRoot)
        || !EqualsOrdinalIgnoreCase(
            configuration.dedicatedRmm,
            canonicalConfiguration.dedicatedRmm)
        || !IsBelow(
            canonicalConfiguration.virtualLogicalSave,
            canonicalConfiguration.virtualDocuments)
        || !IsBelow(
            canonicalConfiguration.dedicatedRmm,
            canonicalConfiguration.externalSaveRoot)) {
        return false;
    }

    const SavePathPolicy policy(SavePathPolicyConfiguration{
        canonicalConfiguration.virtualLogicalSave,
        canonicalConfiguration.realSaveRoot,
        canonicalConfiguration.dedicatedRmm,
    });
    const auto logical = policy.Evaluate(
        PathOperation::Open,
        canonicalConfiguration.virtualLogicalSave);
    const auto normalRoot = policy.Evaluate(
        PathOperation::Open,
        canonicalConfiguration.realSaveRoot);
    return logical.kind == PathDecisionKind::Redirect
        && EqualsOrdinalIgnoreCase(
            logical.EffectivePath(),
            canonicalConfiguration.dedicatedRmm)
        && normalRoot.kind == PathDecisionKind::Deny;
}

void Rollback(
    HookPlatform& platform,
    const bool disable,
    const bool initialized) noexcept {
    if (disable) {
        platform.DisableAll();
    }
    for (std::size_t index = installedTargetCount; index > 0; --index) {
        platform.RemoveHook(installedTargets[index - 1]);
    }
    installedTargetCount = 0;
    if (initialized) {
        platform.Uninitialize();
    }
    activePolicy.reset();
    activeConfiguration = {};
    installedPlatform = nullptr;
    hooksInstalled.store(false, std::memory_order_release);
    ResetOriginals();
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
    if (hooksInstalled.load(std::memory_order_acquire)) {
        return SaveHookInstallStatus::InstallFailed;
    }

    bool platformInitialized = false;
    try {
        SaveHookConfiguration canonicalConfiguration{};
        if (!ValidateConfiguration(configuration, canonicalConfiguration)) {
            return SaveHookInstallStatus::InvalidConfiguration;
        }
        if (!platform.Initialize()) {
            return SaveHookInstallStatus::InstallFailed;
        }
        platformInitialized = true;

        const auto definitions = HookDefinitions();
        std::array<void*, 8> targets{};
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            targets[index] = platform.ResolveTarget(
                definitions[index].module,
                definitions[index].procedure);
            if (targets[index] == nullptr) {
                Rollback(platform, false, true);
                return SaveHookInstallStatus::InstallFailed;
            }
        }

        for (std::size_t index = 0; index < definitions.size(); ++index) {
            if (!platform.CreateHook(
                    targets[index],
                    definitions[index].detour,
                    definitions[index].original)) {
                Rollback(platform, false, true);
                return SaveHookInstallStatus::InstallFailed;
            }
            installedTargets[installedTargetCount++] = targets[index];
        }

        activeConfiguration = canonicalConfiguration;
        activePolicy = std::make_unique<SavePathPolicy>(SavePathPolicyConfiguration{
            activeConfiguration.virtualLogicalSave,
            activeConfiguration.realSaveRoot,
            activeConfiguration.dedicatedRmm,
        });

        for (const auto target : targets) {
            if (!platform.QueueEnable(target)) {
                Rollback(platform, false, true);
                return SaveHookInstallStatus::InstallFailed;
            }
        }
        if (!platform.ApplyQueued()) {
            Rollback(platform, true, true);
            return SaveHookInstallStatus::InstallFailed;
        }

        installedPlatform = &platform;
        hooksInstalled.store(true, std::memory_order_release);
        return SaveHookInstallStatus::Success;
    }
    catch (...) {
        if (platformInitialized) {
            Rollback(platform, true, true);
        }
        return SaveHookInstallStatus::InstallFailed;
    }
}

void UninstallSaveHooks() noexcept {
    std::scoped_lock lock(installMutex);
    if (installedPlatform != nullptr) {
        Rollback(*installedPlatform, true, true);
    }
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

}  // namespace DSRRandomizer::Save

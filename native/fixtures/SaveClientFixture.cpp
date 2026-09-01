#include <Windows.h>
#include <ShlObj.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "DSRRandomizer/ProtectionProtocol.h"

namespace {

using InitializeProtectionFunction = std::uint32_t(__stdcall*)(
    DSRRandomizer::ProtectionInitBlock*);

struct SaveAuditCounters {
    std::uint64_t dedicatedRmm;
    std::uint64_t deniedNormal;
    std::uint64_t deniedOverhaul;
    std::uint64_t unrelated;
};

using QuerySaveAuditCountersFunction = std::uint32_t(__stdcall*)(
    SaveAuditCounters*,
    std::uint32_t);

constexpr std::string_view kSentinel = "rmm-save-hook-sentinel";
constexpr std::string_view kReplacementSentinel = "rmm-replacement-sentinel";

bool CopyWide(
    wchar_t* destination,
    const std::size_t destinationCharacters,
    const std::wstring_view source) {
    if (source.empty() || source.size() >= destinationCharacters) {
        return false;
    }

    std::wmemcpy(destination, source.data(), source.size());
    destination[source.size()] = L'\0';
    return true;
}

bool WriteExact(const std::wstring_view path, const std::string_view value) {
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL wrote = WriteFile(
        file,
        value.data(),
        static_cast<DWORD>(value.size()),
        &written,
        nullptr);
    const BOOL flushed = wrote ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    return wrote && flushed && written == value.size();
}

bool WriteExactWithoutSharing(
    const std::wstring_view path,
    const std::string_view value) {
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD creationStatus = GetLastError();

    DWORD written = 0;
    const BOOL wrote = WriteFile(
        file,
        value.data(),
        static_cast<DWORD>(value.size()),
        &written,
        nullptr);
    const BOOL flushed = wrote ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    return wrote && flushed && written == value.size()
        && creationStatus == ERROR_ALREADY_EXISTS;
}

bool OpenAlwaysReports(
    const std::wstring_view path,
    const DWORD expectedStatus) {
    SetLastError(ERROR_SUCCESS);
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const DWORD status = GetLastError();
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return status == expectedStatus;
}

bool ReadExact(const std::wstring_view path, const std::string_view expected) {
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::array<char, 64> buffer{};
    DWORD read = 0;
    const BOOL readSucceeded = ReadFile(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &read,
        nullptr);
    CloseHandle(file);
    return readSucceeded
        && read == expected.size()
        && std::string_view(buffer.data(), read) == expected;
}

bool ReadExactAnsi(const std::wstring_view path, const std::string_view expected) {
    const int required = WideCharToMultiByte(
        CP_ACP,
        0,
        path.data(),
        static_cast<int>(path.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return false;
    }
    std::string narrow(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_ACP,
            0,
            path.data(),
            static_cast<int>(path.size()),
            narrow.data(),
            required,
            nullptr,
            nullptr) != required) {
        return false;
    }
    const HANDLE file = CreateFileA(
        narrow.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, 64> buffer{};
    DWORD read = 0;
    const BOOL readSucceeded = ReadFile(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &read,
        nullptr);
    CloseHandle(file);
    return readSucceeded
        && read == expected.size()
        && std::string_view(buffer.data(), read) == expected;
}

int ExclusiveOpenResult(const std::wstring_view path) {
    const HANDLE exclusive = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (exclusive == INVALID_HANDLE_VALUE) {
        return 1;
    }
    const HANDLE competing = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const bool rejected = competing == INVALID_HANDLE_VALUE;
    if (competing != INVALID_HANDLE_VALUE) {
        CloseHandle(competing);
    }
    CloseHandle(exclusive);
    return rejected ? 0 : 2;
}

bool IsAccessDeniedCreate(const std::wstring_view path) {
    SetLastError(ERROR_SUCCESS);
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return false;
    }
    return error == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedOpenDirectory(const std::wstring_view path) {
    SetLastError(ERROR_SUCCESS);
    const HANDLE file = CreateFileW(
        std::wstring(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    const DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return false;
    }
    return error == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedDelete(const std::wstring_view path) {
    SetLastError(ERROR_SUCCESS);
    const BOOL deleted = DeleteFileW(std::wstring(path).c_str());
    return !deleted && GetLastError() == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedAttributes(const std::wstring_view path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    SetLastError(ERROR_SUCCESS);
    return !GetFileAttributesExW(
               std::wstring(path).c_str(),
               GetFileExInfoStandard,
               &attributes)
        && GetLastError() == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedFind(const std::wstring_view path) {
    WIN32_FIND_DATAW data{};
    SetLastError(ERROR_SUCCESS);
    const HANDLE find = FindFirstFileExW(
        std::wstring(path).c_str(),
        FindExInfoBasic,
        &data,
        FindExSearchNameMatch,
        nullptr,
        0);
    if (find != INVALID_HANDLE_VALUE) {
        FindClose(find);
        return false;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedMove(
    const std::wstring_view source,
    const std::wstring_view destination) {
    SetLastError(ERROR_SUCCESS);
    return !MoveFileExW(
               std::wstring(source).c_str(),
               std::wstring(destination).c_str(),
               MOVEFILE_REPLACE_EXISTING)
        && GetLastError() == ERROR_ACCESS_DENIED;
}

bool IsAccessDeniedReplace(
    const std::wstring_view replaced,
    const std::wstring_view replacement,
    const wchar_t* backup = nullptr) {
    SetLastError(ERROR_SUCCESS);
    return !ReplaceFileW(
               std::wstring(replaced).c_str(),
               std::wstring(replacement).c_str(),
               backup,
               REPLACEFILE_WRITE_THROUGH,
               nullptr,
               nullptr)
        && GetLastError() == ERROR_ACCESS_DENIED;
}

bool FindOnlyLogicalSave(const std::wstring_view pattern) {
    WIN32_FIND_DATAW data{};
    SetLastError(ERROR_SUCCESS);
    const HANDLE find = FindFirstFileExW(
        std::wstring(pattern).c_str(),
        FindExInfoBasic,
        &data,
        FindExSearchNameMatch,
        nullptr,
        0);
    if (find == INVALID_HANDLE_VALUE
        || std::wstring_view(data.cFileName) != L"DRAKS0005.sl2"
        || data.cAlternateFileName[0] != L'\0') {
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
        }
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const bool noSecondEntry = !FindNextFileW(find, &data)
        && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(find);
    return noSecondEntry;
}

bool FindUnrelatedFile(
    const std::wstring_view pattern,
    const std::wstring_view expectedName) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(
        std::wstring(pattern).c_str(),
        FindExInfoBasic,
        &data,
        FindExSearchNameMatch,
        nullptr,
        0);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool matched = std::wstring_view(data.cFileName) == expectedName;
    FindClose(find);
    return matched;
}

#pragma pack(push, 1)
struct MountPointReparseData {
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
    wchar_t pathBuffer[1];
};
#pragma pack(pop)

bool CreateJunction(
    const std::wstring_view junction,
    const std::wstring_view target) {
    const auto junctionPath = std::wstring(junction);
    const auto attributes = GetFileAttributesW(junctionPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES
        && !RemoveDirectoryW(junctionPath.c_str())) {
        return false;
    }
    if (!CreateDirectoryW(junctionPath.c_str(), nullptr)) {
        return false;
    }

    const auto substitute = L"\\??\\" + std::wstring(target);
    const auto substituteBytes = substitute.size() * sizeof(wchar_t);
    const auto printBytes = target.size() * sizeof(wchar_t);
    const auto printOffset = substituteBytes + sizeof(wchar_t);
    const auto pathBytes = printOffset + printBytes + sizeof(wchar_t);
    const auto dataLength = sizeof(WORD) * 4 + pathBytes;
    if (dataLength > std::numeric_limits<WORD>::max()) {
        return false;
    }

    std::vector<std::byte> buffer(sizeof(MountPointReparseData) + pathBytes);
    auto* data = reinterpret_cast<MountPointReparseData*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(dataLength);
    data->substituteOffset = 0;
    data->substituteLength = static_cast<WORD>(substituteBytes);
    data->printOffset = static_cast<WORD>(printOffset);
    data->printLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    std::memcpy(
        reinterpret_cast<std::byte*>(data->pathBuffer) + printOffset,
        target.data(),
        printBytes);

    const HANDLE directory = CreateFileW(
        std::wstring(junction).c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL created = DeviceIoControl(
        directory,
        FSCTL_SET_REPARSE_POINT,
        data,
        static_cast<DWORD>(sizeof(DWORD) + sizeof(WORD) * 2 + dataLength),
        nullptr,
        0,
        &returned,
        nullptr);
    CloseHandle(directory);
    return created != FALSE;
}

bool CreateDirectoryIfMissing(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr)
        || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool RemoveFileIfPresent(const std::wstring& path) {
    return DeleteFileW(path.c_str())
        || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool GuardedFileApisAreDenied(
    const std::wstring& guardedRoot,
    const std::wstring& physicalRoot,
    const std::wstring& scratchRoot) {
    const auto createPath = guardedRoot + L"\\create.bin";
    const auto deletePath = guardedRoot + L"\\delete.bin";
    const auto movePath = guardedRoot + L"\\move.bin";
    const auto replacePath = guardedRoot + L"\\replace.bin";
    const auto attributesPath = guardedRoot + L"\\attributes.bin";
    const auto findPath = guardedRoot + L"\\find.bin";
    const auto moveDestination = scratchRoot + L"\\moved.bin";
    const auto replacement = scratchRoot + L"\\replacement.bin";

    if (!WriteExact(physicalRoot + L"\\delete.bin", "delete")
        || !WriteExact(physicalRoot + L"\\move.bin", "move")
        || !WriteExact(physicalRoot + L"\\replace.bin", "replace")
        || !WriteExact(physicalRoot + L"\\attributes.bin", "attributes")
        || !WriteExact(physicalRoot + L"\\find.bin", "find")
        || !WriteExact(replacement, "replacement")) {
        return false;
    }

    const bool createDenied = IsAccessDeniedCreate(createPath);
    const bool deleteDenied = IsAccessDeniedDelete(deletePath);

    SetLastError(ERROR_SUCCESS);
    const bool moveDenied = !MoveFileExW(
            movePath.c_str(),
            moveDestination.c_str(),
            MOVEFILE_REPLACE_EXISTING)
        && GetLastError() == ERROR_ACCESS_DENIED;

    SetLastError(ERROR_SUCCESS);
    const bool replaceDenied = !ReplaceFileW(
            replacePath.c_str(),
            replacement.c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr)
        && GetLastError() == ERROR_ACCESS_DENIED;
    const bool attributesDenied = IsAccessDeniedAttributes(attributesPath);
    const bool findDenied = IsAccessDeniedFind(findPath);

    return createDenied
        && deleteDenied
        && moveDenied
        && replaceDenied
        && attributesDenied
        && findDenied;
}

bool CleanupGuardedApiFiles(
    const std::wstring& physicalRoot,
    const std::wstring& scratchRoot) {
    const std::array<std::wstring_view, 6> names{
        L"create.bin", L"delete.bin", L"move.bin", L"replace.bin",
        L"attributes.bin", L"find.bin"};
    bool cleaned = true;
    for (const auto name : names) {
        cleaned = RemoveFileIfPresent(physicalRoot + L"\\" + std::wstring(name))
            && cleaned;
    }
    cleaned = RemoveFileIfPresent(scratchRoot + L"\\moved.bin") && cleaned;
    cleaned = RemoveFileIfPresent(scratchRoot + L"\\replacement.bin") && cleaned;
    return cleaned;
}

bool MoveFileOutsideHooks(
    const std::wstring& source,
    const std::wstring& destination) {
    using MoveFileExFunction = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
    const auto kernelBase = GetModuleHandleW(L"KernelBase.dll");
    const auto moveFileEx = kernelBase == nullptr
        ? nullptr
        : reinterpret_cast<MoveFileExFunction>(
            GetProcAddress(kernelBase, "MoveFileExW"));
    return moveFileEx != nullptr
        && moveFileEx(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING);
}

int VerifyGuardedReparseFailures(
    const std::wstring& virtualDocuments,
    const std::wstring& logicalSave,
    const std::wstring& realSave,
    const std::wstring& externalRoot,
    const std::wstring& escapeTarget) {
    const auto realProfileSeparator = realSave.find_last_of(L'\\');
    const auto realProfile = realSave.substr(0, realProfileSeparator);
    const auto realRootSeparator = realProfile.find_last_of(L'\\');
    const auto realRoot = realProfile.substr(0, realRootSeparator);

    const auto virtualTarget = escapeTarget + L"\\virtual-target";
    const auto virtualScratch = escapeTarget + L"\\virtual-scratch";
    const auto savedVirtual = escapeTarget + L"\\virtual-original";
    const auto stagedVirtual = escapeTarget + L"\\virtual-link-stage";
    if (!CreateDirectoryIfMissing(virtualTarget)
        || !CreateDirectoryIfMissing(virtualScratch)
        || !CreateJunction(stagedVirtual, virtualTarget)
        || !MoveFileOutsideHooks(virtualDocuments, savedVirtual)
        || !MoveFileOutsideHooks(stagedVirtual, virtualDocuments)) {
        return 50;
    }
    const bool virtualDenied = GuardedFileApisAreDenied(
        virtualDocuments,
        virtualTarget,
        virtualScratch);
    if (!RemoveDirectoryW(virtualDocuments.c_str())
        || !CleanupGuardedApiFiles(virtualTarget, virtualScratch)
        || !RemoveDirectoryW(virtualTarget.c_str())
        || !RemoveDirectoryW(virtualScratch.c_str())
        || !MoveFileOutsideHooks(savedVirtual, virtualDocuments)) {
        return 51;
    }
    if (!virtualDenied) {
        return 52;
    }

    const auto realTarget = escapeTarget + L"\\real-target";
    const auto realScratch = escapeTarget + L"\\real-scratch";
    const auto savedReal = escapeTarget + L"\\real-original";
    const auto stagedReal = escapeTarget + L"\\real-link-stage";
    if (!CreateDirectoryIfMissing(realTarget)
        || !CreateDirectoryIfMissing(realScratch)
        || !CreateJunction(stagedReal, realTarget)
        || !MoveFileOutsideHooks(realRoot, savedReal)
        || !MoveFileOutsideHooks(stagedReal, realRoot)) {
        return 53;
    }
    const bool realDenied = GuardedFileApisAreDenied(
        realRoot,
        realTarget,
        realScratch);
    if (!RemoveDirectoryW(realRoot.c_str())
        || !CleanupGuardedApiFiles(realTarget, realScratch)
        || !RemoveDirectoryW(realTarget.c_str())
        || !RemoveDirectoryW(realScratch.c_str())
        || !MoveFileOutsideHooks(savedReal, realRoot)) {
        return 54;
    }
    if (!realDenied) {
        return 55;
    }

    const auto finalTarget = escapeTarget + L"\\final-target";
    const auto stagedFinal = escapeTarget + L"\\final-link-stage";
    const auto finalMove = externalRoot + L"\\final-move.bin";
    const auto finalReplacement = externalRoot + L"\\final-replacement.bin";
    if (!CreateDirectoryIfMissing(finalTarget)
        || !WriteExact(finalReplacement, "final-replacement")
        || !CreateJunction(stagedFinal, finalTarget)
        || !MoveFileOutsideHooks(stagedFinal, logicalSave)) {
        return 56;
    }
    const bool finalOpenDenied = IsAccessDeniedOpenDirectory(logicalSave);
    const bool finalDeleteDenied = IsAccessDeniedDelete(logicalSave);
    const bool finalMoveDenied = IsAccessDeniedMove(logicalSave, finalMove);
    const bool finalReplaceDenied = IsAccessDeniedReplace(
        logicalSave,
        finalReplacement);
    const bool finalAttributesDenied = IsAccessDeniedAttributes(logicalSave);
    const bool finalFindDenied = IsAccessDeniedFind(logicalSave);
    const bool finalDenied = finalOpenDenied
        && finalDeleteDenied
        && finalMoveDenied
        && finalReplaceDenied
        && finalAttributesDenied
        && finalFindDenied;
    if (!RemoveDirectoryW(logicalSave.c_str())
        || !RemoveDirectoryW(finalTarget.c_str())
        || !RemoveFileIfPresent(finalMove)
        || !RemoveFileIfPresent(finalReplacement)) {
        return 57;
    }
    if (!finalDenied) {
        return 58;
    }

    const auto danglingTarget = escapeTarget + L"\\dangling-target";
    const auto stagedDangling = escapeTarget + L"\\dangling-link-stage";
    const auto danglingMove = externalRoot + L"\\dangling-move.bin";
    const auto danglingReplacement = externalRoot + L"\\dangling-replacement.bin";
    if (!CreateDirectoryIfMissing(danglingTarget)
        || !WriteExact(danglingReplacement, "dangling-replacement")
        || !CreateJunction(stagedDangling, danglingTarget)
        || !MoveFileOutsideHooks(stagedDangling, logicalSave)
        || !RemoveDirectoryW(danglingTarget.c_str())) {
        return 59;
    }
    const bool danglingCreateDenied = IsAccessDeniedCreate(logicalSave);
    const bool danglingDeleteDenied = IsAccessDeniedDelete(logicalSave);
    const bool danglingMoveDenied = IsAccessDeniedMove(logicalSave, danglingMove);
    const bool danglingReplaceDenied = IsAccessDeniedReplace(
        logicalSave,
        danglingReplacement);
    const bool danglingAttributesDenied = IsAccessDeniedAttributes(logicalSave);
    const bool danglingFindDenied = IsAccessDeniedFind(logicalSave);
    const bool danglingDenied = danglingCreateDenied
        && danglingDeleteDenied
        && danglingMoveDenied
        && danglingReplaceDenied
        && danglingAttributesDenied
        && danglingFindDenied;
    if (!RemoveDirectoryW(logicalSave.c_str())
        || !RemoveFileIfPresent(danglingMove)
        || !RemoveFileIfPresent(danglingReplacement)) {
        return 60;
    }
    if (!danglingDenied) {
        return 61;
    }

    const auto unrelatedTarget = escapeTarget + L"\\unrelated-target";
    const auto unrelatedLink = escapeTarget + L"\\unrelated-link";
    const auto renamedLink = escapeTarget + L"\\renamed-link";
    if (!CreateDirectoryIfMissing(unrelatedTarget)
        || !CreateJunction(unrelatedLink, unrelatedTarget)
        || !MoveFileExW(
            unrelatedLink.c_str(),
            renamedLink.c_str(),
            MOVEFILE_REPLACE_EXISTING)) {
        return 62;
    }
    const auto renamedAttributes = GetFileAttributesW(renamedLink.c_str());
    const bool callerPathWasPreserved = renamedAttributes != INVALID_FILE_ATTRIBUTES
        && (renamedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        && GetFileAttributesW(unrelatedLink.c_str()) == INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW(unrelatedTarget.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!callerPathWasPreserved) {
        return 63;
    }
    if (!RemoveDirectoryW(renamedLink.c_str())
        || !RemoveDirectoryW(unrelatedTarget.c_str())) {
        return 64;
    }
    return 0;
}

int RunFileOperations(
    const std::wstring& virtualDocuments,
    const std::wstring& logicalSave,
    const std::wstring& realSave,
    const std::wstring& overhaulSave,
    const std::wstring& externalRoot,
    const std::wstring& escapeTarget) {
    PWSTR knownDocuments = nullptr;
    const auto knownFolderResult = SHGetKnownFolderPath(
        FOLDERID_Documents,
        KF_FLAG_DEFAULT,
        nullptr,
        &knownDocuments);
    const bool knownFolderWasVirtualized = SUCCEEDED(knownFolderResult)
        && knownDocuments != nullptr
        && std::wstring_view(knownDocuments) == virtualDocuments;
    CoTaskMemFree(knownDocuments);
    if (!knownFolderWasVirtualized) {
        return 20;
    }

    std::array<wchar_t, 1024> legacyDocuments{};
    if (FAILED(SHGetFolderPathW(
            nullptr,
            CSIDL_PERSONAL,
            nullptr,
            SHGFP_TYPE_CURRENT,
            legacyDocuments.data()))
        || std::wstring_view(legacyDocuments.data()) != virtualDocuments) {
        return 21;
    }

    const auto externalOriginal = escapeTarget + L"\\external-original";
    const auto stagedExternal = escapeTarget + L"\\external-link-stage";
    if (!CreateJunction(stagedExternal, escapeTarget)) {
        return 33;
    }
    SetLastError(ERROR_SUCCESS);
    const bool guardedRootMoveWasDenied = !MoveFileW(
        externalRoot.c_str(),
        externalOriginal.c_str())
        && GetLastError() == ERROR_ACCESS_DENIED;
    if (!RemoveDirectoryW(stagedExternal.c_str())) {
        return 34;
    }
    const auto externalAttributes = GetFileAttributesW(externalRoot.c_str());
    if (!guardedRootMoveWasDenied
        || externalAttributes == INVALID_FILE_ATTRIBUTES
        || (externalAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return 35;
    }

    const auto dedicatedSave = externalRoot + L"\\DRAKS0005.rmm";
    const auto protectedTarget = escapeTarget + L"\\hardlink-protected.bin";
    if (!WriteExact(protectedTarget, kReplacementSentinel)
        || !DeleteFileW(dedicatedSave.c_str())
        || !CreateHardLinkW(
            dedicatedSave.c_str(),
            protectedTarget.c_str(),
            nullptr)) {
        return 79;
    }
    if (WriteExactWithoutSharing(logicalSave, kSentinel)
        || !ReadExact(protectedTarget, kReplacementSentinel)) {
        return 80;
    }
    if (!DeleteFileW(dedicatedSave.c_str())
        || !WriteExact(dedicatedSave, kSentinel)
        || !DeleteFileW(protectedTarget.c_str())) {
        return 81;
    }

    if (!WriteExactWithoutSharing(logicalSave, kSentinel)) {
        return 22;
    }
    if (!ReadExact(logicalSave, kSentinel)) {
        return 76;
    }
    if (!ReadExactAnsi(logicalSave, kSentinel)) {
        return 88;
    }
    if (GetFileAttributesW(logicalSave.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 84;
    }
    WIN32_FIND_DATAW directFindData{};
    const HANDLE directFind = FindFirstFileW(
        logicalSave.c_str(),
        &directFindData);
    if (directFind == INVALID_HANDLE_VALUE) {
        return 85;
    }
    FindClose(directFind);
    if (std::wstring_view(directFindData.cFileName) != L"DRAKS0005.sl2") {
        return 86;
    }
    const auto directMoved = externalRoot + L"\\direct-move-stage.tmp";
    if (!MoveFileW(logicalSave.c_str(), directMoved.c_str())
        || !MoveFileW(directMoved.c_str(), logicalSave.c_str())) {
        return 87;
    }
    if (!OpenAlwaysReports(logicalSave, ERROR_ALREADY_EXISTS)) {
        return 82;
    }
    if (!SetFileAttributesW(dedicatedSave.c_str(), FILE_ATTRIBUTE_HIDDEN)
        || !IsAccessDeniedCreate(logicalSave)
        || !ReadExact(dedicatedSave, kSentinel)
        || !SetFileAttributesW(dedicatedSave.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        return 83;
    }
    const auto exclusiveResult = ExclusiveOpenResult(logicalSave);
    if (exclusiveResult != 0) {
        return 76 + exclusiveResult;
    }

    const auto realProfileSeparator = realSave.find_last_of(L'\\');
    const auto realProfile = realSave.substr(0, realProfileSeparator);
    const auto realRootSeparator = realProfile.find_last_of(L'\\');
    const auto realRoot = realProfile.substr(0, realRootSeparator);
    const auto outsideAlias = escapeTarget + L"\\outside-real-alias";
    if (!CreateJunction(outsideAlias, realRoot)) {
        return 73;
    }
    const bool outsideAliasDenied = IsAccessDeniedAttributes(
        outsideAlias + L"\\alias-target.bin");
    if (!RemoveDirectoryW(outsideAlias.c_str())) {
        return 74;
    }
    if (!outsideAliasDenied) {
        return 75;
    }

    const auto profileSeparator = logicalSave.find_last_of(L'\\');
    const auto logicalProfile = logicalSave.substr(0, profileSeparator);
    const auto hiddenPhysical = externalRoot + L"\\physical-only.tmp";
    const auto unrelatedDirectory = escapeTarget + L"\\unrelated-enumeration";
    const auto unrelatedFile = unrelatedDirectory + L"\\caller-visible.tmp";
    if (!WriteExact(hiddenPhysical, "hidden")
        || !CreateDirectoryIfMissing(unrelatedDirectory)
        || !WriteExact(unrelatedFile, "unrelated")) {
        return 65;
    }
    if (!FindOnlyLogicalSave(logicalProfile + L"\\*")) {
        return 70;
    }
    if (!FindOnlyLogicalSave(logicalProfile + L"\\DRAKS0005.*")) {
        return 71;
    }
    if (!FindUnrelatedFile(
            unrelatedDirectory + L"\\*.tmp",
            L"caller-visible.tmp")) {
        return 72;
    }
    if (!RemoveFileIfPresent(hiddenPhysical)
        || !RemoveFileIfPresent(unrelatedFile)
        || !RemoveDirectoryW(unrelatedDirectory.c_str())) {
        return 66;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(
            logicalSave.c_str(),
            GetFileExInfoStandard,
            &attributes)
        || attributes.nFileSizeLow != kSentinel.size()
        || attributes.nFileSizeHigh != 0) {
        return 23;
    }

    WIN32_FIND_DATAW findData{};
    const HANDLE find = FindFirstFileExW(
        logicalSave.c_str(),
        FindExInfoBasic,
        &findData,
        FindExSearchNameMatch,
        nullptr,
        0);
    if (find == INVALID_HANDLE_VALUE) {
        return 24;
    }
    FindClose(find);
    if (std::wstring_view(findData.cFileName) != L"DRAKS0005.sl2") {
        return 25;
    }

    const auto outsideMoveDestination = escapeTarget + L"\\outside-move.tmp";
    const auto outsideMoveSource = escapeTarget + L"\\outside-source.tmp";
    const auto outsideReplacement = escapeTarget + L"\\outside-replacement.tmp";
    const auto outsideBackup = escapeTarget + L"\\outside-backup.tmp";
    const auto guardedReplacement = externalRoot + L"\\guarded-replacement.tmp";
    if (!WriteExact(outsideMoveSource, "outside-source")
        || !WriteExact(outsideReplacement, "outside-replacement")
        || !WriteExact(guardedReplacement, "guarded-replacement")) {
        return 67;
    }
    if (!IsAccessDeniedMove(logicalSave, outsideMoveDestination)
        || !IsAccessDeniedMove(outsideMoveSource, logicalSave)
        || !IsAccessDeniedReplace(logicalSave, outsideReplacement)
        || !IsAccessDeniedReplace(
            logicalSave,
            guardedReplacement,
            outsideBackup.c_str())) {
        return 68;
    }
    if (!RemoveFileIfPresent(outsideMoveSource)
        || !RemoveFileIfPresent(outsideReplacement)
        || !RemoveFileIfPresent(guardedReplacement)
        || !RemoveFileIfPresent(outsideMoveDestination)
        || !RemoveFileIfPresent(outsideBackup)) {
        return 69;
    }

    const auto moved = externalRoot + L"\\move-stage.tmp";
    if (!MoveFileExW(logicalSave.c_str(), moved.c_str(), MOVEFILE_REPLACE_EXISTING)
        || !MoveFileExW(moved.c_str(), logicalSave.c_str(), MOVEFILE_REPLACE_EXISTING)
        || !ReadExact(logicalSave, kSentinel)) {
        return 26;
    }

    const auto replacement = externalRoot + L"\\replacement.tmp";
    if (!WriteExact(replacement, kReplacementSentinel)
        || !ReplaceFileW(
            logicalSave.c_str(),
            replacement.c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr)
        || !ReadExact(logicalSave, kReplacementSentinel)) {
        return 27;
    }

    if (!DeleteFileW(logicalSave.c_str())
        || GetFileAttributesW(logicalSave.c_str()) != INVALID_FILE_ATTRIBUTES
        || !OpenAlwaysReports(logicalSave, ERROR_SUCCESS)
        || !WriteExact(logicalSave, kSentinel)) {
        return 28;
    }

    const auto unrelated = externalRoot + L"\\unrelated.tmp";
    if (!WriteExact(unrelated, "unrelated") || !DeleteFileW(unrelated.c_str())) {
        return 29;
    }

    if (!IsAccessDeniedCreate(realSave) || !IsAccessDeniedCreate(overhaulSave)) {
        return 30;
    }

    const auto separator = logicalSave.find_last_of(L'\\');
    const auto shortNameSave = logicalSave.substr(0, separator)
        + L"\\PROFIL~1\\DRAKS0005.sl2";
    if (!IsAccessDeniedCreate(shortNameSave)) {
        return 31;
    }

    const auto reparseResult = VerifyGuardedReparseFailures(
        virtualDocuments,
        logicalSave,
        realSave,
        externalRoot,
        escapeTarget);
    if (reparseResult != 0) {
        return reparseResult;
    }

    return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 9) {
        return 2;
    }

    const HMODULE guard = LoadLibraryW(argv[1]);
    if (guard == nullptr) {
        return 3;
    }

    const auto initialize = reinterpret_cast<InitializeProtectionFunction>(
        GetProcAddress(guard, "InitializeProtection"));
    const auto queryAudit = reinterpret_cast<QuerySaveAuditCountersFunction>(
        GetProcAddress(guard, "QuerySaveAuditCounters"));
    if (initialize == nullptr || queryAudit == nullptr) {
        return 4;
    }

    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::SaveKnownFolder)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::SaveFileIo)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::Heartbeat)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::HookIntegrity);
    if (!CopyWide(
            block.pipeName,
            DSRRandomizer::kProtectionPipeNameCharacters,
            argv[2])
        || !CopyWide(
            block.virtualDocuments,
            DSRRandomizer::kProtectionSavePathCharacters,
            argv[3])
        || !CopyWide(
            block.virtualLogicalSave,
            DSRRandomizer::kProtectionSavePathCharacters,
            argv[4])
        || !CopyWide(
            block.realSaveRoot,
            DSRRandomizer::kProtectionSavePathCharacters,
            argv[5])
        || !CopyWide(
            block.externalSaveRoot,
            DSRRandomizer::kProtectionSavePathCharacters,
            argv[7])
        || !CopyWide(
            block.dedicatedRmm,
            DSRRandomizer::kProtectionSavePathCharacters,
            std::wstring(argv[7]) + L"\\DRAKS0005.rmm")) {
        return 5;
    }

    const auto initializeStatus = initialize(&block);
    if (initializeStatus != 0) {
        return static_cast<int>(40 + initializeStatus);
    }

    const auto realSave = std::wstring(argv[5])
        + L"\\12345678901234567\\DRAKS0005.sl2";
    const auto operationResult = RunFileOperations(
        argv[3], argv[4], realSave, argv[6], argv[7], argv[8]);
    if (operationResult != 0) {
        return operationResult;
    }

    SaveAuditCounters counters{};
    if (queryAudit(&counters, sizeof(counters)) != ERROR_SUCCESS
        || counters.dedicatedRmm == 0
        || counters.deniedNormal == 0
        || counters.deniedOverhaul == 0
        || counters.unrelated == 0) {
        return 32;
    }
    return 0;
}

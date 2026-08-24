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
    if (!RemoveDirectoryW(std::wstring(junction).c_str())
        || !CreateDirectoryW(std::wstring(junction).c_str(), nullptr)) {
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

    if (!CreateJunction(externalRoot, escapeTarget)) {
        return 33;
    }
    const bool escapeWasDenied = IsAccessDeniedCreate(logicalSave);
    if (!RemoveDirectoryW(externalRoot.c_str())
        || !CreateDirectoryW(externalRoot.c_str(), nullptr)) {
        return 34;
    }
    if (!escapeWasDenied) {
        return 35;
    }

    if (!WriteExact(logicalSave, kSentinel)
        || !ReadExact(logicalSave, kSentinel)) {
        return 22;
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
            DSRRandomizer::ProtectionFlags::SaveFileIo);
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

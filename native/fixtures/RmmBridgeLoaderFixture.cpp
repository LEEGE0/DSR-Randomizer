#include <Windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>

using ModEngineInit = bool (*)(void*, void**);

std::wstring Join(std::wstring left, const std::wstring& right) {
    left.push_back(L'\\');
    left.append(right);
    return left;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 2;
    }
    PWSTR realDocumentsValue = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &realDocumentsValue))) {
        return 10;
    }
    const std::wstring realDocuments(realDocumentsValue);
    CoTaskMemFree(realDocumentsValue);

    const auto bridge = LoadLibraryW(argv[1]);
    if (bridge == nullptr) {
        return 3;
    }
    const auto initialize = reinterpret_cast<ModEngineInit>(
        GetProcAddress(bridge, "modengine_ext_init"));
    if (initialize == nullptr) {
        return 4;
    }
    void* extension = reinterpret_cast<void*>(1);
    if (initialize(nullptr, &extension) || extension != nullptr) {
        return 5;
    }

    PWSTR documentsValue = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsValue))) {
        return 6;
    }
    const std::wstring documents(documentsValue);
    CoTaskMemFree(documentsValue);
    if (documents.find(L"\\profile") == std::wstring::npos) {
        return 7;
    }
    const auto logicalSave = Join(
        Join(Join(Join(documents, L"NBGI"), L"DARK SOULS REMASTERED"),
             L"146808034"),
        L"DRAKS0005.sl2");
    const auto save = CreateFileW(
        logicalSave.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (save == INVALID_HANDLE_VALUE) {
        return 8;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = 128;
    const unsigned char marker = 0x5a;
    DWORD written{};
    const auto changed = SetFilePointerEx(save, offset, nullptr, FILE_BEGIN)
        && WriteFile(save, &marker, 1, &written, nullptr)
        && written == 1
        && FlushFileBuffers(save);
    CloseHandle(save);
    if (!changed) {
        return 9;
    }

    const auto normalSave = Join(
        Join(Join(Join(realDocuments, L"NBGI"), L"DARK SOULS REMASTERED"),
             L"146808034"),
        L"DRAKS0005.sl2");
    SetLastError(ERROR_SUCCESS);
    const auto denied = CreateFileW(
        normalSave.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (denied != INVALID_HANDLE_VALUE) {
        CloseHandle(denied);
        return 11;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED) {
        return 12;
    }
    return 0;
}

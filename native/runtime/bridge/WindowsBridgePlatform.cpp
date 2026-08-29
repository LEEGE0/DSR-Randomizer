#include "bridge/WindowsBridgePlatform.h"

#include <Windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

#include "bridge/RmmBridgeHostClient.h"

namespace DSRRandomizer::Bridge {
namespace {

constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

std::wstring NormalizeFinalPath(std::wstring value) {
    constexpr std::wstring_view uncPrefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view devicePrefix = LR"(\\?\)";
    if (value.starts_with(uncPrefix)) {
        value = L"\\\\" + value.substr(uncPrefix.size());
    } else if (value.starts_with(devicePrefix)) {
        value.erase(0, devicePrefix.size());
    }
    return value;
}

std::wstring Parent(std::wstring value) {
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    const auto separator = value.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : value.substr(0, separator);
}

std::wstring Leaf(std::wstring value) {
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    const auto separator = value.find_last_of(L"\\/");
    return separator == std::wstring::npos ? value : value.substr(separator + 1);
}

std::wstring DeriveExternalRoot(std::wstring processImage) {
    const auto runtimeRoot = Parent(std::move(processImage));
    const auto runtimesRoot = Parent(runtimeRoot);
    if (_wcsicmp(Leaf(runtimesRoot).c_str(), L"runtimes") != 0) {
        return {};
    }
    return Parent(runtimesRoot);
}

bool ReadAll(HANDLE file, std::vector<unsigned char>& bytes) {
    DWORD read{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>(1U << 20)));
        if (!ReadFile(file, bytes.data() + offset, chunk, &read, nullptr)
            || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

}  // namespace

std::wstring WindowsBridgePlatform::ProcessImagePath() const {
    std::vector<wchar_t> buffer(512);
    while (true) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring WindowsBridgePlatform::DocumentsPath() const {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &value))) {
        return {};
    }
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

bool WindowsBridgePlatform::ReadBoundedUtf8(
    const std::wstring& path,
    const std::size_t maximumBytes,
    std::string& content) const {
    const auto file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const auto valid = GetFileInformationByHandle(file, &information);
    const auto length = valid
        ? (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32)
            | information.nFileSizeLow
        : maximumBytes + 1;
    const auto attributes = static_cast<DWORD>(information.dwFileAttributes);
    if (!valid || length > maximumBytes
        || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || information.nNumberOfLinks != 1) {
        CloseHandle(file);
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    const auto read = bytes.empty() || ReadAll(file, bytes);
    CloseHandle(file);
    if (!read) {
        return false;
    }
    content.assign(bytes.begin(), bytes.end());
    return true;
}

bool WindowsBridgePlatform::CanonicalizeExisting(
    const std::wstring& path,
    std::wstring& canonical) const {
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, ShareAll, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::vector<wchar_t> buffer(512);
    while (true) {
        const auto length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
        if (length == 0) {
            CloseHandle(handle);
            return false;
        }
        if (length < buffer.size()) {
            canonical = NormalizeFinalPath(std::wstring(buffer.data(), length));
            CloseHandle(handle);
            return true;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

bool WindowsBridgePlatform::InspectFile(
    const std::wstring& path,
    FileInspection& inspection) const {
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, ShareAll, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        CloseHandle(handle);
        return false;
    }
    CloseHandle(handle);
    const auto attributes = static_cast<DWORD>(information.dwFileAttributes);
    inspection = FileInspection{
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
        true,
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
        information.nNumberOfLinks,
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32)
            | information.nFileSizeLow,
    };
    return true;
}

bool WindowsBridgePlatform::Sha256File(
    const std::wstring& path,
    std::string& sha256) const {
    const auto file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength{};
    DWORD received{};
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest{};
    auto ok = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) {
        ok = BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &received, 0) >= 0;
    }
    if (ok) {
        object.resize(objectLength);
        ok = BCryptCreateHash(
            algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    while (ok) {
        DWORD read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok) {
        ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto value : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    sha256 = stream.str();
    return true;
}

BridgeConfigurationResult WindowsBridgePlatform::ResolveConfiguration() {
    return ResolveBridgeConfiguration(*this);
}

bool WindowsBridgePlatform::StartHostAndWaitReady(
    const BridgeConfiguration& configuration,
    std::wstring& message) {
    return StartRmmBridgeHostAndWaitReady(configuration, message);
}

bool WindowsBridgePlatform::InstallHooks(
    const Save::SaveHookConfiguration& configuration,
    std::wstring& message) {
    if (Save::InstallSaveHooks(configuration) != Save::SaveHookInstallStatus::Success) {
        message = L"The RMM save hooks could not be installed.";
        return false;
    }
    return true;
}

void WindowsBridgePlatform::WriteFailureLog(
    const BridgeConfiguration* configuration,
    const std::wstring_view message) {
    const auto externalRoot = configuration != nullptr
        ? configuration->externalRoot
        : DeriveExternalRoot(ProcessImagePath());
    if (externalRoot.empty()) {
        return;
    }
    const auto logs = externalRoot + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    const auto path = logs + L"\\rmm-bridge.log";
    const auto file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::wostringstream line;
    line << std::setfill(L'0')
         << std::setw(4) << time.wYear << L'-'
         << std::setw(2) << time.wMonth << L'-'
         << std::setw(2) << time.wDay << L'T'
         << std::setw(2) << time.wHour << L':'
         << std::setw(2) << time.wMinute << L':'
         << std::setw(2) << time.wSecond << L"Z " << message << L"\r\n";
    const auto wide = line.str();
    const auto bytesRequired = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytesRequired > 0) {
        std::string utf8(static_cast<std::size_t>(bytesRequired), '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            utf8.data(), bytesRequired, nullptr, nullptr);
        DWORD written{};
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
}

}  // namespace DSRRandomizer::Bridge

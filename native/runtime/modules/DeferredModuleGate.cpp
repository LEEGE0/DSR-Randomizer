#include "modules/DeferredModuleGate.h"

#include <Windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <utility>

#include "hooks/MinHookCoordinator.h"
#include "steam/SteamPolicy.h"

namespace DSRRandomizer::Modules {
namespace {

using LoadLibraryAFunction = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFunction = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFunction = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFunction = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetProcAddressFunction = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using LoaderCalloutProbeFunction = BOOLEAN(NTAPI*)();
using LdrLoadDllFunction = NTSTATUS(NTAPI*)(
    PWSTR,
    PULONG,
    PUNICODE_STRING,
    PHANDLE);
using LdrGetProcedureAddressFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*);
using LdrGetProcedureAddressExFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*,
    ULONG);
using LdrGetProcedureAddressForCallerFunction = NTSTATUS(NTAPI*)(
    PVOID,
    PANSI_STRING,
    ULONG,
    PVOID*,
    ULONG,
    PVOID);

constexpr NTSTATUS kNativeAccessDenied = static_cast<NTSTATUS>(0xc0000022U);
constexpr NTSTATUS kNativeSuccess = 0;

struct UniqueHandle final {
    HANDLE value = INVALID_HANDLE_VALUE;

    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : value(handle) {}
    ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr) {
                CloseHandle(value);
            }
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

struct PinnedModule final {
    HMODULE value = nullptr;

    PinnedModule() = default;
    explicit PinnedModule(const HMODULE module) noexcept : value(module) {}
    ~PinnedModule() {
        if (value != nullptr) {
            FreeLibrary(value);
        }
    }
    PinnedModule(const PinnedModule&) = delete;
    PinnedModule& operator=(const PinnedModule&) = delete;
    PinnedModule(PinnedModule&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    PinnedModule& operator=(PinnedModule&& other) noexcept {
        if (this != &other) {
            if (value != nullptr) {
                FreeLibrary(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};

struct ProtectedExport {
    std::string name;
    std::size_t slot = Steam::kSteamFactorySlotCapacity;
    void* target = nullptr;
};

struct ModuleRecord {
    std::wstring canonicalPath;
    std::wstring baseName;
    UniqueHandle expectedFile;
    FILE_ID_INFO expectedIdentity{};
    std::array<std::uint8_t, 32> expectedSha256{};
    bool allowDeferred = false;
    std::vector<std::string> declaredInterfaces;
    std::vector<ProtectedExport> protectedExports;
    std::mutex admissionMutex;
    HMODULE admittedModule = nullptr;
    PinnedModule pinnedModule;
    bool admitted = false;
};

struct Trampolines {
    LoadLibraryAFunction loadLibraryA = nullptr;
    LoadLibraryWFunction loadLibraryW = nullptr;
    LoadLibraryExAFunction loadLibraryExA = nullptr;
    LoadLibraryExWFunction loadLibraryExW = nullptr;
    GetProcAddressFunction getProcAddress = nullptr;
    LdrLoadDllFunction ldrLoadDll = nullptr;
    LdrGetProcedureAddressFunction ldrGetProcedureAddress = nullptr;
    LdrGetProcedureAddressExFunction ldrGetProcedureAddressEx = nullptr;
    LdrGetProcedureAddressForCallerFunction
        ldrGetProcedureAddressForCaller = nullptr;
};

struct GateContext {
    std::shared_ptr<void> identityLease;
    std::vector<std::unique_ptr<ModuleRecord>> modules;
    std::shared_ptr<Steam::FatalState> fatalState;
    Trampolines trampolines;
    LoaderCalloutProbeFunction loaderCalloutProbe = nullptr;
    std::mutex slotMutex;
    std::array<bool, Steam::kSteamFactorySlotCapacity> slots{};
    std::mutex quarantineMutex;
    std::vector<HMODULE> quarantinedModules;
    Steam::SteamInterfaceLayout interfaceLayout =
        Steam::SteamInterfaceLayout::ProductionPinned;

    ~GateContext() {
        for (const auto module : quarantinedModules) {
            if (module != nullptr) {
                FreeLibrary(module);
            }
        }
    }
};

struct HookEntry {
    void* target = nullptr;
    bool created = false;
    bool factory = false;
    std::size_t factorySlot = Steam::kSteamFactorySlotCapacity;
    bool enabled = false;
};

struct Lifecycle {
    std::shared_ptr<GateContext> context;
    std::vector<HookEntry> hooks;
    bool initialized = false;
    bool coverageComplete = false;
};

std::atomic<std::shared_ptr<GateContext>> activeContext;
std::atomic<bool> gateInstalled{false};
std::atomic<std::uint32_t> failFactoryPublication{};
std::atomic<std::uint32_t> failFactoryPostCreate{};
std::atomic<HANDLE> afterInitialDisableEvent{};
std::atomic<HANDLE> beforeFactoryDrainEvent{};
std::atomic<HANDLE> afterFactoryApplyEvent{};
std::atomic<HANDLE> allowFactoryRollbackEvent{};
std::atomic<Testing::CountedStringSnapshotHook> countedStringSnapshotHook{};
Lifecycle lifecycle;
std::mutex installMutex;
std::mutex hooksMutex;
std::recursive_mutex loaderCallbackMutex;
std::shared_mutex callbackGate;
thread_local std::uint32_t callbackDepth = 0;
thread_local std::uint32_t internalBypassDepth = 0;
thread_local std::uint32_t nativeLoadDelegationAllowance = 0;
thread_local std::uint32_t nativeSymbolDelegationAllowance = 0;

void SignalTestEvent(const std::atomic<HANDLE>& event) noexcept {
    const auto handle = event.load(std::memory_order_acquire);
    if (handle != nullptr) {
        SetEvent(handle);
    }
}

class InternalBypass final {
public:
    InternalBypass() noexcept { ++internalBypassDepth; }
    ~InternalBypass() { --internalBypassDepth; }
    InternalBypass(const InternalBypass&) = delete;
    InternalBypass& operator=(const InternalBypass&) = delete;
};

class OneShotNativeDelegation final {
public:
    explicit OneShotNativeDelegation(std::uint32_t& allowance) noexcept
        : allowance_(allowance), previous_(allowance) {
        allowance_ = 1;
    }
    ~OneShotNativeDelegation() { allowance_ = previous_; }
    OneShotNativeDelegation(const OneShotNativeDelegation&) = delete;
    OneShotNativeDelegation& operator=(const OneShotNativeDelegation&) = delete;

private:
    std::uint32_t& allowance_;
    std::uint32_t previous_;
};

bool ConsumeNativeDelegation(std::uint32_t& allowance) noexcept {
    if (allowance == 0) {
        return false;
    }
    --allowance;
    return true;
}

class CallbackLease final {
public:
    CallbackLease() {
        nested_ = callbackDepth++ != 0;
        if (!nested_) {
            callbackLock_.emplace(callbackGate);
            loaderLock_.emplace(loaderCallbackMutex);
        }
    }
    ~CallbackLease() { --callbackDepth; }
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
    [[nodiscard]] bool IsNested() const noexcept { return nested_; }

private:
    bool nested_ = false;
    std::optional<std::unique_lock<std::recursive_mutex>> loaderLock_;
    std::optional<std::shared_lock<std::shared_mutex>> callbackLock_;
};

bool SafeWritePointer(void** const destination, void* const value) noexcept {
    if (destination == nullptr) {
        return false;
    }
    __try {
        *destination = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void InvokeCountedStringSnapshotHook() noexcept {
    const auto hook = countedStringSnapshotHook.exchange(
        nullptr,
        std::memory_order_acq_rel);
    if (hook != nullptr) {
        hook();
    }
}

bool SafeCopyNativeUnicode(
    const UNICODE_STRING* const source,
    wchar_t* const destination,
    const std::size_t capacity,
    std::size_t& length) noexcept {
    if (source == nullptr || destination == nullptr || capacity == 0) {
        return false;
    }
    PWSTR buffer = nullptr;
    USHORT byteLength = 0;
    USHORT maximumByteLength = 0;
    __try {
        buffer = source->Buffer;
        byteLength = source->Length;
        maximumByteLength = source->MaximumLength;
        InvokeCountedStringSnapshotHook();
        if (buffer == nullptr || byteLength == 0
            || (byteLength % sizeof(wchar_t)) != 0
            || byteLength > maximumByteLength) {
            return false;
        }
        const auto characters = static_cast<std::size_t>(
            byteLength / sizeof(wchar_t));
        if (characters >= capacity) {
            return false;
        }
        std::memcpy(
            destination,
            buffer,
            characters * sizeof(wchar_t));
        destination[characters] = L'\0';
        length = characters;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopyNativeAnsi(
    const ANSI_STRING* const source,
    char* const destination,
    const std::size_t capacity,
    std::size_t& length) noexcept {
    if (source == nullptr || destination == nullptr || capacity == 0) {
        return false;
    }
    PCHAR buffer = nullptr;
    USHORT byteLength = 0;
    USHORT maximumByteLength = 0;
    __try {
        buffer = source->Buffer;
        byteLength = source->Length;
        maximumByteLength = source->MaximumLength;
        InvokeCountedStringSnapshotHook();
        if (buffer == nullptr || byteLength == 0
            || byteLength > maximumByteLength
            || byteLength >= capacity) {
            return false;
        }
        std::memcpy(destination, buffer, byteLength);
        destination[byteLength] = '\0';
        length = byteLength;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyNativePath(
    const UNICODE_STRING* const source,
    std::wstring& path) noexcept {
    try {
        std::array<wchar_t, 32768> buffer{};
        std::size_t length = 0;
        if (!SafeCopyNativeUnicode(
                source,
                buffer.data(),
                buffer.size(),
                length)
            || std::find(buffer.begin(), buffer.begin() + length, L'\0')
                != buffer.begin() + length) {
            return false;
        }
        path.assign(buffer.data(), length);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool CopyNativeProcedureName(
    const ANSI_STRING* const source,
    std::string& name) noexcept {
    try {
        std::array<char, 4096> buffer{};
        std::size_t length = 0;
        if (!SafeCopyNativeAnsi(
                source,
                buffer.data(),
                buffer.size(),
                length)
            || std::find(buffer.begin(), buffer.begin() + length, '\0')
                != buffer.begin() + length) {
            return false;
        }
        name.assign(buffer.data(), length);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool EqualsPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(INT_MAX)
        || right.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::wstring BaseName(const std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    return Lowercase(std::wstring(
        slash == std::wstring_view::npos ? path : path.substr(slash + 1)));
}

bool ReadModulePath(const HMODULE module, std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        module,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

bool ReadMappedDevicePath(const HMODULE module, std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetMappedFileNameW(
        GetCurrentProcess(),
        reinterpret_cast<void*>(module),
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return path.starts_with(L"\\Device\\");
}

bool ReadCanonicalPath(const HANDLE file, std::wstring& path) {
    const auto required = GetFinalPathNameByHandleW(file, nullptr, 0, 0);
    if (required == 0) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
    const auto length = GetFinalPathNameByHandleW(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        0);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

UniqueHandle OpenPinnedReadOnly(const std::wstring& path) noexcept {
    return UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
}

UniqueHandle OpenMappedBacking(const std::wstring& devicePath) noexcept {
    const std::wstring globalPath = L"\\\\?\\GLOBALROOT" + devicePath;
    return OpenPinnedReadOnly(globalPath);
}

bool ReadFileIdentity(const HANDLE file, FILE_ID_INFO& identity) noexcept {
    return GetFileInformationByHandleEx(
        file,
        FileIdInfo,
        &identity,
        sizeof(identity)) != FALSE;
}

bool SameFileIdentity(
    const FILE_ID_INFO& first,
    const FILE_ID_INFO& second) noexcept {
    return first.VolumeSerialNumber == second.VolumeSerialNumber
        && std::memcmp(
            first.FileId.Identifier,
            second.FileId.Identifier,
            sizeof(first.FileId.Identifier)) == 0;
}

bool PinModule(const HMODULE module, PinnedModule& pinned) noexcept {
    HMODULE reference = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module),
            &reference)
        || reference != module) {
        if (reference != nullptr) {
            FreeLibrary(reference);
        }
        return false;
    }
    pinned = PinnedModule(reference);
    return true;
}

bool IsCanonicalAbsolutePath(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':'
        || (path[2] != L'\\' && path[2] != L'/')
        || path.find(L'~') != std::wstring::npos) {
        return false;
    }
    std::vector<wchar_t> buffer(32768);
    const auto length = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    return length != 0 && length < buffer.size()
        && EqualsPath(path, std::wstring_view(buffer.data(), length));
}

bool HashFileHandle(
    const HANDLE file,
    std::array<std::uint8_t, 32>& digest) noexcept {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD bytes = 0;
    std::vector<std::uint8_t> object;
    bool ok = BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) >= 0
        && BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytes,
            0) >= 0;
    try {
        if (ok) {
            object.resize(objectLength);
            ok = BCryptCreateHash(
                algorithm,
                &hash,
                object.data(),
                static_cast<ULONG>(object.size()),
                nullptr,
                0,
                0) >= 0;
        }
        std::array<std::uint8_t, 4096> buffer{};
        while (ok) {
            DWORD read = 0;
            if (!ReadFile(
                    file,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
        }
        if (ok) {
            ok = BCryptFinishHash(
                hash,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0) >= 0;
        }
    }
    catch (...) {
        ok = false;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

bool ReadWholeFile(
    const HANDLE file,
    std::vector<std::uint8_t>& bytes) {
    constexpr std::uint64_t kMaximumPreflightImageSize = 512ULL * 1024ULL * 1024ULL;
    LARGE_INTEGER size{};
    LARGE_INTEGER beginning{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) > kMaximumPreflightImageSize
        || !SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto remaining = bytes.size() - total;
        const auto request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + total, request, &read, nullptr)
            || read == 0) {
            return false;
        }
        total += read;
    }
    return true;
}

template <typename Value>
bool ReadImageValue(
    const std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    Value& value) noexcept {
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return true;
}

struct PeImageView final {
    const std::vector<std::uint8_t>* bytes = nullptr;
    std::vector<IMAGE_SECTION_HEADER> sections;
    std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES>
        directories{};
    std::uint32_t sizeOfHeaders = 0;
    std::uint64_t imageBase = 0;

    [[nodiscard]] std::optional<std::size_t> FileOffset(
        const std::uint32_t rva,
        const std::size_t length) const noexcept {
        if (bytes == nullptr) {
            return std::nullopt;
        }
        if (rva < sizeOfHeaders) {
            const auto offset = static_cast<std::size_t>(rva);
            return offset <= bytes->size() && length <= bytes->size() - offset
                ? std::optional<std::size_t>(offset)
                : std::nullopt;
        }
        for (const auto& section : sections) {
            const std::uint64_t start = section.VirtualAddress;
            const std::uint64_t span = (std::max)(
                section.Misc.VirtualSize,
                section.SizeOfRawData);
            const std::uint64_t requested = rva;
            if (requested < start || requested - start > span) {
                continue;
            }
            const auto delta = requested - start;
            if (delta > section.SizeOfRawData
                || length > section.SizeOfRawData - delta) {
                return std::nullopt;
            }
            const auto rawOffset = static_cast<std::uint64_t>(
                    section.PointerToRawData)
                + delta;
            if (rawOffset > bytes->size()
                || length > bytes->size() - rawOffset) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(rawOffset);
        }
        return std::nullopt;
    }
};

bool BuildPeImageView(
    const std::vector<std::uint8_t>& bytes,
    PeImageView& view) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadImageValue(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew < 0) {
        return false;
    }
    const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader{};
    if (!ReadImageValue(bytes, ntOffset, signature)
        || signature != IMAGE_NT_SIGNATURE
        || ntOffset > bytes.size() - sizeof(signature)
        || !ReadImageValue(
            bytes,
            ntOffset + sizeof(signature),
            fileHeader)
        || fileHeader.NumberOfSections == 0
        || fileHeader.NumberOfSections > 96) {
        return false;
    }
    const auto optionalOffset = ntOffset + sizeof(signature)
        + sizeof(fileHeader);
    WORD magic = 0;
    if (!ReadImageValue(bytes, optionalOffset, magic)) {
        return false;
    }
    DWORD directoryCount = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (fileHeader.SizeOfOptionalHeader < sizeof(optional)
            || !ReadImageValue(bytes, optionalOffset, optional)) {
            return false;
        }
        view.sizeOfHeaders = optional.SizeOfHeaders;
        view.imageBase = optional.ImageBase;
        directoryCount = optional.NumberOfRvaAndSizes;
        std::copy(
            std::begin(optional.DataDirectory),
            std::end(optional.DataDirectory),
            view.directories.begin());
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (fileHeader.SizeOfOptionalHeader < sizeof(optional)
            || !ReadImageValue(bytes, optionalOffset, optional)) {
            return false;
        }
        view.sizeOfHeaders = optional.SizeOfHeaders;
        view.imageBase = optional.ImageBase;
        directoryCount = optional.NumberOfRvaAndSizes;
        std::copy(
            std::begin(optional.DataDirectory),
            std::end(optional.DataDirectory),
            view.directories.begin());
    }
    else {
        return false;
    }
    if (directoryCount > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) {
        directoryCount = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    }
    for (std::size_t index = directoryCount;
         index < view.directories.size();
         ++index) {
        view.directories[index] = {};
    }
    const auto sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    view.sections.reserve(fileHeader.NumberOfSections);
    for (std::size_t index = 0; index < fileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        const auto offset = sectionsOffset + index * sizeof(section);
        if (!ReadImageValue(bytes, offset, section)) {
            return false;
        }
        view.sections.push_back(section);
    }
    view.bytes = &bytes;
    return true;
}

bool ReadImportName(
    const PeImageView& image,
    const std::uint32_t nameRva,
    std::wstring& name) {
    constexpr std::size_t kMaximumImportNameLength = 4096;
    name.clear();
    for (std::size_t index = 0; index < kMaximumImportNameLength; ++index) {
        if (nameRva > (std::numeric_limits<std::uint32_t>::max)() - index) {
            return false;
        }
        const auto characterRva = nameRva + static_cast<std::uint32_t>(index);
        const auto offset = image.FileOffset(characterRva, 1);
        if (!offset.has_value()) {
            return false;
        }
        const auto character = (*image.bytes)[*offset];
        if (character == 0) {
            return !name.empty();
        }
        if (character > 0x7f) {
            return false;
        }
        name.push_back(static_cast<wchar_t>(character));
    }
    return false;
}

template <typename Descriptor, typename NameRva>
bool ReadImportDirectory(
    const PeImageView& image,
    const IMAGE_DATA_DIRECTORY& directory,
    NameRva&& nameRva,
    std::vector<std::wstring>& imports) {
    if (directory.VirtualAddress == 0 && directory.Size == 0) {
        return true;
    }
    if (directory.VirtualAddress == 0
        || directory.Size < sizeof(Descriptor)) {
        return false;
    }
    const auto maximum = directory.Size / sizeof(Descriptor);
    for (std::size_t index = 0; index < maximum; ++index) {
        const auto descriptorRva = static_cast<std::uint64_t>(
                directory.VirtualAddress)
            + index * sizeof(Descriptor);
        if (descriptorRva > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        const auto offset = image.FileOffset(
            static_cast<std::uint32_t>(descriptorRva),
            sizeof(Descriptor));
        Descriptor descriptor{};
        if (!offset.has_value()
            || !ReadImageValue(*image.bytes, *offset, descriptor)) {
            return false;
        }
        const auto rva = nameRva(descriptor, image.imageBase);
        if (!rva.has_value()) {
            return false;
        }
        if (*rva == 0) {
            return true;
        }
        std::wstring import;
        if (!ReadImportName(image, *rva, import)) {
            return false;
        }
        imports.push_back(BaseName(import));
    }
    return false;
}

struct DelayImportDescriptor final {
    DWORD attributes;
    DWORD name;
    DWORD moduleHandle;
    DWORD importAddressTable;
    DWORD importNameTable;
    DWORD boundImportAddressTable;
    DWORD unloadInformationTable;
    DWORD timeStamp;
};

bool ReadPeImports(
    const HANDLE file,
    std::vector<std::wstring>& imports) {
    std::vector<std::uint8_t> bytes;
    PeImageView image;
    if (!ReadWholeFile(file, bytes) || !BuildPeImageView(bytes, image)) {
        return false;
    }
    const bool normal = ReadImportDirectory<IMAGE_IMPORT_DESCRIPTOR>(
        image,
        image.directories[IMAGE_DIRECTORY_ENTRY_IMPORT],
        [](const IMAGE_IMPORT_DESCRIPTOR& descriptor, const std::uint64_t) {
            return std::optional<std::uint32_t>(descriptor.Name);
        },
        imports);
    if (!normal) {
        return false;
    }
    return ReadImportDirectory<DelayImportDescriptor>(
        image,
        image.directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT],
        [](const DelayImportDescriptor& descriptor, const std::uint64_t imageBase)
            -> std::optional<std::uint32_t> {
            constexpr DWORD kRvaAttribute = 1;
            if ((descriptor.attributes & ~kRvaAttribute) != 0) {
                return std::nullopt;
            }
            if ((descriptor.attributes & kRvaAttribute) != 0
                || descriptor.name == 0) {
                return descriptor.name;
            }
            if (descriptor.name < imageBase
                || descriptor.name - imageBase
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(descriptor.name - imageBase);
        },
        imports);
}

bool Fatal(
    const std::shared_ptr<GateContext>& context,
    const char* const code) noexcept {
    if (context != nullptr && context->fatalState != nullptr) {
        context->fatalState->Trigger(code);
    }
    return false;
}

bool IsFatal(const std::shared_ptr<GateContext>& context) noexcept {
    return context == nullptr || context->fatalState == nullptr
        || context->fatalState->IsFatal();
}

FARPROC CallOriginalGetProcAddress(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module,
    const LPCSTR procedureName) noexcept {
    if (context == nullptr || context->trampolines.getProcAddress == nullptr
        || IsFatal(context)) {
        return nullptr;
    }
    OneShotNativeDelegation delegation(nativeSymbolDelegationAllowance);
    const auto result = context->trampolines.getProcAddress(
        module,
        procedureName);
    return IsFatal(context) ? nullptr : result;
}

bool IsWithinLoaderCallout(
    const std::shared_ptr<GateContext>& context) noexcept {
    if (context == nullptr || context->loaderCalloutProbe == nullptr) {
        static_cast<void>(Fatal(context, "STEAM_LOADER_PROVENANCE_UNAVAILABLE"));
        return true;
    }
    return context->loaderCalloutProbe() != FALSE;
}

void Quarantine(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) noexcept {
    if (context == nullptr || module == nullptr) {
        return;
    }
    try {
        std::scoped_lock lock(context->quarantineMutex);
        context->quarantinedModules.push_back(module);
    }
    catch (...) {
        // A non-resumable fatal has already been established. Leaking the
        // withheld loader reference is safer than publishing or unloading it.
    }
}

std::size_t AllocateFactorySlot(GateContext& context) noexcept {
    std::scoped_lock lock(context.slotMutex);
    const auto available = std::find(
        context.slots.begin(),
        context.slots.end(),
        false);
    if (available == context.slots.end()) {
        return Steam::kSteamFactorySlotCapacity;
    }
    *available = true;
    return static_cast<std::size_t>(available - context.slots.begin());
}

void ReleaseFactorySlot(GateContext& context, const std::size_t slot) noexcept {
    std::scoped_lock lock(context.slotMutex);
    if (slot < context.slots.size()) {
        context.slots[slot] = false;
    }
}

bool ConsumeFactoryPublicationFault() noexcept {
    auto value = failFactoryPublication.load(std::memory_order_acquire);
    while (value != 0) {
        if (failFactoryPublication.compare_exchange_weak(
                value,
                value - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool ConsumeFactoryPostCreateFault() noexcept {
    auto value = failFactoryPostCreate.load(std::memory_order_acquire);
    while (value != 0) {
        if (failFactoryPostCreate.compare_exchange_weak(
                value,
                value - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool AllCreatedHooksEnabledLocked() noexcept {
    bool found = false;
    for (const auto& hook : lifecycle.hooks) {
        if (!hook.created) {
            continue;
        }
        found = true;
        if (!hook.enabled) {
            return false;
        }
    }
    return found;
}

bool EnsureRetainedHooksEnabled() noexcept {
    std::vector<std::size_t> queued;
    bool complete = true;
    try {
        std::scoped_lock lifecycleLock(hooksMutex);
        queued.reserve(lifecycle.hooks.size());
        for (std::size_t index = 0; index < lifecycle.hooks.size(); ++index) {
            auto& hook = lifecycle.hooks[index];
            if (!hook.created || hook.enabled) {
                continue;
            }
            if (Hooks::QueueEnableHook(hook.target) == MH_OK) {
                queued.push_back(index);
            }
            else {
                complete = false;
            }
        }
        if (!queued.empty()) {
            if (Hooks::ApplyQueuedHooks() == MH_OK) {
                for (const auto index : queued) {
                    lifecycle.hooks[index].enabled = true;
                }
            }
            else {
                complete = false;
            }
        }
        return complete && AllCreatedHooksEnabledLocked();
    }
    catch (...) {
        return false;
    }
}

bool RollBackFactoryHooks(
    GateContext& context,
    std::vector<HookEntry>& created) noexcept {
    {
        Hooks::MinHookMutationLease mutation;
        for (const auto& hook : created) {
            if (hook.created) {
                static_cast<void>(Hooks::DisableHook(hook.target));
            }
        }
    }
    SignalTestEvent(beforeFactoryDrainEvent);
    Steam::SteamFactoryCallbackBlock factoryCallbackBlock;
    std::array<HookEntry, Steam::kSteamFactorySlotCapacity> retained{};
    std::size_t retainedCount = 0;
    bool removedAny = false;
    {
        Hooks::MinHookMutationLease mutation;
        for (std::size_t index = created.size(); index > 0; --index) {
            auto& hook = created[index - 1];
            const auto disable = Hooks::DisableHook(hook.target);
            const bool disabled = disable == MH_OK
                || disable == MH_ERROR_DISABLED
                || disable == MH_ERROR_NOT_CREATED;
            const auto remove = disabled
                ? Hooks::RemoveHook(hook.target)
                : MH_ERROR_ENABLED;
            if (remove == MH_OK || remove == MH_ERROR_NOT_CREATED) {
                Steam::UnregisterSteamFactorySlot(hook.factorySlot);
                ReleaseFactorySlot(context, hook.factorySlot);
                hook.created = false;
                removedAny = true;
            }
            else {
                retained[retainedCount++] = hook;
            }
        }
    }
    if (retainedCount != 0) {
        {
            std::scoped_lock lifecycleLock(hooksMutex);
            if (removedAny) {
                lifecycle.coverageComplete = false;
            }
            lifecycle.hooks.insert(
                lifecycle.hooks.end(),
                retained.begin(),
                retained.begin() + static_cast<std::ptrdiff_t>(retainedCount));
        }
        Hooks::MinHookMutationLease mutation;
        static_cast<void>(EnsureRetainedHooksEnabled());
    }
    created.clear();
    return retainedCount == 0;
}

void RetainAppliedFactoryHooks(std::vector<HookEntry>& created) noexcept {
    std::scoped_lock lifecycleLock(hooksMutex);
    for (auto& hook : created) {
        hook.enabled = true;
    }
    lifecycle.hooks.insert(
        lifecycle.hooks.end(),
        created.begin(),
        created.end());
    created.clear();
}

bool LifecycleOwnsFactoryHooks() noexcept {
    std::scoped_lock lifecycleLock(hooksMutex);
    return std::any_of(
        lifecycle.hooks.begin(),
        lifecycle.hooks.end(),
        [](const HookEntry& hook) {
            return hook.created
                && hook.factorySlot < Steam::kSteamFactorySlotCapacity;
        });
}

bool ProtectFactoryExports(
    const std::shared_ptr<GateContext>& context,
    ModuleRecord& record,
    const HMODULE module) noexcept {
    std::vector<HookEntry> created;
    bool complete = true;
    bool applied = false;
    try {
        created.reserve(record.protectedExports.size());
        Hooks::MinHookMutationLease mutation;
        for (auto& exportEntry : record.protectedExports) {
            const auto slot = AllocateFactorySlot(*context);
            if (slot >= Steam::kSteamFactorySlotCapacity
                || Steam::RegisterSteamFactorySlot(
                    slot,
                    record.declaredInterfaces,
                    context->fatalState,
                    context->interfaceLayout)
                    != Steam::SteamFactorySlotStatus::Success) {
                if (slot < Steam::kSteamFactorySlotCapacity) {
                    ReleaseFactorySlot(*context, slot);
                }
                complete = false;
                break;
            }

            void* const target = reinterpret_cast<void*>(
                CallOriginalGetProcAddress(
                    context,
                    module,
                    exportEntry.name.c_str()));
            void* const detour = Steam::SteamFactoryDetourAddress(slot);
            void* original = nullptr;
            created.push_back({target, false, true, slot});
            if (target == nullptr || detour == nullptr
                || Hooks::CreateHook(target, detour, &original) != MH_OK) {
                created.pop_back();
                Steam::UnregisterSteamFactorySlot(slot);
                ReleaseFactorySlot(*context, slot);
                complete = false;
                break;
            }
            created.back().created = true;
            if (ConsumeFactoryPostCreateFault()) {
                complete = false;
                break;
            }
            if (!Steam::SetSteamFactoryOriginal(
                    slot,
                    reinterpret_cast<Steam::Synthetic::FactoryFunction>(
                        original))
                || Hooks::QueueEnableHook(target) != MH_OK) {
                complete = false;
                break;
            }
            exportEntry.slot = slot;
            exportEntry.target = target;
        }

        if (complete && created.size() == record.protectedExports.size()) {
            complete = Hooks::ApplyQueuedHooks() == MH_OK;
            applied = complete;
            if (applied) {
                for (auto& hook : created) {
                    hook.enabled = true;
                }
            }
        }
        if (complete) {
            SignalTestEvent(afterFactoryApplyEvent);
            const auto allow = allowFactoryRollbackEvent.load(
                std::memory_order_acquire);
            if (allow != nullptr) {
                WaitForSingleObject(allow, INFINITE);
            }
        }
        if (complete && ConsumeFactoryPublicationFault()) {
            complete = false;
        }
        if (complete && !IsFatal(context)) {
            std::scoped_lock lifecycleLock(hooksMutex);
            lifecycle.hooks.insert(
                lifecycle.hooks.end(),
                created.begin(),
                created.end());
            return true;
        }
    }
    catch (...) {
        complete = false;
    }
    context->fatalState->EnterDenyOnly();
    if (applied) {
        RetainAppliedFactoryHooks(created);
    }
    else {
        static_cast<void>(RollBackFactoryHooks(*context, created));
    }
    return Fatal(context, "STEAM_HOOK_FAILED");
}

struct AdmissionResult {
    bool target = false;
    bool admitted = false;
    ModuleRecord* record = nullptr;
};

AdmissionResult AdmitModuleCore(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) {
    if (context == nullptr || module == nullptr) {
        return {};
    }
    if (IsFatal(context)) {
        return {true, false, nullptr};
    }
    const auto alreadyAdmitted = std::find_if(
        context->modules.begin(),
        context->modules.end(),
        [module](const std::unique_ptr<ModuleRecord>& record) {
            std::scoped_lock admissionLock(record->admissionMutex);
            return record->admitted && record->admittedModule == module;
        });
    if (alreadyAdmitted != context->modules.end()) {
        return IsFatal(context)
            ? AdmissionResult{true, false, alreadyAdmitted->get()}
            : AdmissionResult{true, true, alreadyAdmitted->get()};
    }

    PinnedModule pinned;
    if (!PinModule(module, pinned)) {
        return {true, Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE"), nullptr};
    }
    std::wstring loaderPath;
    if (!ReadModulePath(pinned.value, loaderPath)) {
        return {true, Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE"), nullptr};
    }
    const auto baseName = BaseName(loaderPath);
    const auto found = std::find_if(
        context->modules.begin(),
        context->modules.end(),
        [&baseName](const std::unique_ptr<ModuleRecord>& record) {
            return record->baseName == baseName;
        });
    if (found == context->modules.end()) {
        return {};
    }

    ModuleRecord& record = **found;
    std::scoped_lock admissionLock(record.admissionMutex);
    if (record.admitted) {
        return record.admittedModule == module && !IsFatal(context)
            ? AdmissionResult{true, true, &record}
            : AdmissionResult{
                true,
                Fatal(context, "STEAM_MODULE_PATH_MISMATCH"),
                &record};
    }
    if (IsFatal(context)) {
        return {true, false, &record};
    }

    InternalBypass bypass;
    std::wstring mappedNameBefore;
    if (!ReadMappedDevicePath(pinned.value, mappedNameBefore)) {
        return {
            true,
            Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE"),
            &record};
    }
    auto backingFile = OpenMappedBacking(mappedNameBefore);
    std::wstring mappedNameAfter;
    FILE_ID_INFO actualIdentity{};
    std::wstring canonicalActual;
    if (backingFile.value == INVALID_HANDLE_VALUE
        || !ReadMappedDevicePath(pinned.value, mappedNameAfter)
        || !EqualsPath(mappedNameBefore, mappedNameAfter)
        || !ReadFileIdentity(backingFile.value, actualIdentity)
        || !SameFileIdentity(actualIdentity, record.expectedIdentity)
        || !ReadCanonicalPath(backingFile.value, canonicalActual)
        || !EqualsPath(canonicalActual, record.canonicalPath)) {
        return {true, Fatal(context, "STEAM_MODULE_PATH_MISMATCH"), &record};
    }
    std::array<std::uint8_t, 32> actualHash{};
    if (!HashFileHandle(backingFile.value, actualHash)
        || !std::equal(
            actualHash.begin(),
            actualHash.end(),
            record.expectedSha256.begin())) {
        return {true, Fatal(context, "STEAM_MODULE_HASH_MISMATCH"), &record};
    }
    // Lifecycle ownership of the module must precede factory-hook application:
    // a post-apply failure retains the detour and can hand failure back while a
    // callback is still executing code from this mapped image.
    record.pinnedModule = std::move(pinned);
    record.admittedModule = module;
    if (IsFatal(context) || !ProtectFactoryExports(context, record, module)
        || IsFatal(context)) {
        return {true, false, &record};
    }
    record.admitted = true;
    return {true, true, &record};
}

AdmissionResult AdmitModule(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) noexcept {
    try {
        return AdmitModuleCore(context, module);
    }
    catch (...) {
        return {true, Fatal(context, "STEAM_GATE_UNAVAILABLE"), nullptr};
    }
}

bool RequestedTarget(
    const std::shared_ptr<GateContext>& context,
    const std::wstring_view requested) noexcept {
    try {
        if (context == nullptr || requested.empty()) {
            return false;
        }
        const auto baseName = BaseName(requested);
        return std::any_of(
            context->modules.begin(),
            context->modules.end(),
            [&baseName](const std::unique_ptr<ModuleRecord>& record) {
                return record->baseName == baseName;
            });
    }
    catch (...) {
        static_cast<void>(Fatal(context, "STEAM_GATE_UNAVAILABLE"));
        return true;
    }
}

bool RequestedTarget(
    const std::shared_ptr<GateContext>& context,
    const char* const requested) noexcept {
    try {
        if (requested == nullptr || *requested == '\0') {
            return false;
        }
        const auto required = MultiByteToWideChar(
            CP_ACP,
            MB_ERR_INVALID_CHARS,
            requested,
            -1,
            nullptr,
            0);
        if (required <= 1) {
            return false;
        }
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        if (MultiByteToWideChar(
                CP_ACP,
                MB_ERR_INVALID_CHARS,
                requested,
                -1,
                wide.data(),
                required) != required) {
            return false;
        }
        wide.resize(static_cast<std::size_t>(required - 1));
        return RequestedTarget(context, wide);
    }
    catch (...) {
        static_cast<void>(Fatal(context, "STEAM_GATE_UNAVAILABLE"));
        return true;
    }
}

bool ModuleIsRequestedTarget(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) noexcept {
    try {
        std::wstring path;
        return module != nullptr && ReadModulePath(module, path)
            && RequestedTarget(context, path);
    }
    catch (...) {
        static_cast<void>(Fatal(context, "STEAM_GATE_UNAVAILABLE"));
        return true;
    }
}

bool EnumerateMatchingModules(
    const std::wstring& baseName,
    std::vector<PinnedModule>& matches) noexcept {
    try {
        UniqueHandle snapshot;
        for (std::size_t retry = 0; retry < 8; ++retry) {
            snapshot = UniqueHandle(CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                GetCurrentProcessId()));
            if (snapshot.value != INVALID_HANDLE_VALUE
                || GetLastError() != ERROR_BAD_LENGTH) {
                break;
            }
        }
        if (snapshot.value == INVALID_HANDLE_VALUE) {
            return false;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.value, &entry)) {
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
        do {
            if (BaseName(entry.szModule) != baseName) {
                continue;
            }
            PinnedModule pinned;
            if (!PinModule(entry.hModule, pinned)) {
                return false;
            }
            matches.push_back(std::move(pinned));
        } while (Module32NextW(snapshot.value, &entry));
        return GetLastError() == ERROR_NO_MORE_FILES;
    }
    catch (...) {
        return false;
    }
}

bool RequestedPath(const wchar_t* const requested, std::wstring& path) {
    if (requested == nullptr || *requested == L'\0') {
        return false;
    }
    path = requested;
    return true;
}

bool RequestedPath(const std::wstring_view requested, std::wstring& path) {
    if (requested.empty()) {
        return false;
    }
    path.assign(requested);
    return true;
}

bool RequestedPath(const char* const requested, std::wstring& path) {
    if (requested == nullptr || *requested == '\0') {
        return false;
    }
    const auto required = MultiByteToWideChar(
        CP_ACP,
        MB_ERR_INVALID_CHARS,
        requested,
        -1,
        nullptr,
        0);
    if (required <= 1) {
        return false;
    }
    path.resize(static_cast<std::size_t>(required));
    if (MultiByteToWideChar(
            CP_ACP,
            MB_ERR_INVALID_CHARS,
            requested,
            -1,
            path.data(),
            required) != required) {
        return false;
    }
    path.resize(static_cast<std::size_t>(required - 1));
    return true;
}

bool VerifyRequestedProtectedFile(
    const std::shared_ptr<GateContext>& context,
    const std::wstring& requestedPath,
    const HANDLE file) {
    const auto baseName = BaseName(requestedPath);
    const auto found = std::find_if(
        context->modules.begin(),
        context->modules.end(),
        [&baseName](const std::unique_ptr<ModuleRecord>& record) {
            return record->baseName == baseName;
        });
    if (found == context->modules.end()) {
        return Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE");
    }
    ModuleRecord& record = **found;
    std::wstring canonical;
    FILE_ID_INFO identity{};
    if (!ReadCanonicalPath(file, canonical)
        || !ReadFileIdentity(file, identity)
        || !EqualsPath(canonical, record.canonicalPath)
        || !SameFileIdentity(identity, record.expectedIdentity)) {
        return Fatal(context, "STEAM_MODULE_PATH_MISMATCH");
    }
    std::array<std::uint8_t, 32> actualHash{};
    if (!HashFileHandle(file, actualHash)
        || !std::equal(
            actualHash.begin(),
            actualHash.end(),
            record.expectedSha256.begin())) {
        return Fatal(context, "STEAM_MODULE_HASH_MISMATCH");
    }
    return !IsFatal(context);
}

bool VerifyUnprotectedImportBoundary(
    const std::shared_ptr<GateContext>& context,
    const HANDLE file) {
    std::vector<std::wstring> imports;
    if (!ReadPeImports(file, imports)) {
        return Fatal(context, "STEAM_DEPENDENCY_PREFLIGHT_UNAVAILABLE");
    }
    for (const auto& import : imports) {
        if (std::any_of(
                context->modules.begin(),
                context->modules.end(),
                [&import](const std::unique_ptr<ModuleRecord>& record) {
                    return record->baseName == import;
                })) {
            return Fatal(
                context,
                "STEAM_PROTECTED_IMPORT_PREFLIGHT_DENIED");
        }
    }
    // This Task 3 boundary permits an unprotected outer image only when all of
    // its direct normal/delay dependencies are already loaded. Consequently no
    // new unprotected intermediate (and no hidden protected descendant) can run
    // DllMain before the outer LoadLibrary returns. Task 4 may replace this
    // conservative rule with a profile-pinned recursive closure.
    for (const auto& import : imports) {
        // API-set contract names are virtual loader contracts, not physical
        // module basenames. Their already-loaded host DLL is selected by the
        // Windows loader schema, so physical-module enumeration cannot and must
        // not require a one-to-one match for the contract name.
        if (import.starts_with(L"api-ms-win-")
            || import.starts_with(L"ext-ms-win-")) {
            continue;
        }
        std::vector<PinnedModule> matches;
        if (!EnumerateMatchingModules(import, matches)
            || matches.size() != 1) {
            return Fatal(context, "STEAM_DEPENDENCY_CLOSURE_UNAVAILABLE");
        }
    }
    return !IsFatal(context);
}

template <typename Path>
bool PreflightLoad(
    const std::shared_ptr<GateContext>& context,
    const Path path,
    const bool requestedTarget,
    UniqueHandle& pinnedFile) noexcept {
    try {
        std::wstring requestedPath;
        if (!RequestedPath(path, requestedPath)
            || !IsCanonicalAbsolutePath(requestedPath)) {
            return Fatal(context, "STEAM_DEPENDENCY_PREFLIGHT_UNAVAILABLE");
        }
        auto file = OpenPinnedReadOnly(requestedPath);
        if (file.value == INVALID_HANDLE_VALUE) {
            return Fatal(
                context,
                requestedTarget
                    ? "STEAM_MODULE_IDENTITY_UNAVAILABLE"
                    : "STEAM_DEPENDENCY_PREFLIGHT_UNAVAILABLE");
        }
        const bool verified = requestedTarget
            ? VerifyRequestedProtectedFile(context, requestedPath, file.value)
                && VerifyUnprotectedImportBoundary(context, file.value)
            : VerifyUnprotectedImportBoundary(context, file.value);
        if (verified) {
            pinnedFile = std::move(file);
        }
        return verified;
    }
    catch (...) {
        return Fatal(context, "STEAM_DEPENDENCY_PREFLIGHT_UNAVAILABLE");
    }
}

bool ScanAndAdmitExpectedModules(
    const std::shared_ptr<GateContext>& context,
    const bool requireEager) noexcept {
    for (const auto& record : context->modules) {
        std::vector<PinnedModule> matches;
        if (!EnumerateMatchingModules(record->baseName, matches)) {
            return Fatal(context, "STEAM_MODULE_ENUMERATION_FAILED");
        }
        if (matches.size() > 1) {
            return Fatal(context, "STEAM_MODULE_DUPLICATE_BASENAME");
        }
        if (matches.size() == 1) {
            const auto admission = AdmitModule(context, matches.front().value);
            if (!admission.target || !admission.admitted) {
                return false;
            }
        }
        else if (requireEager || !record->allowDeferred) {
            return Fatal(context, "STEAM_MODULE_REQUIRED_EAGER");
        }
    }
    return !IsFatal(context);
}

bool AdmitAfterLoad(
    const std::shared_ptr<GateContext>& context,
    const HMODULE returnedModule,
    const bool requestedTarget) noexcept {
    const auto returnedAdmission = AdmitModule(context, returnedModule);
    if (returnedAdmission.target && !returnedAdmission.admitted) {
        return false;
    }
    if (!returnedAdmission.target && requestedTarget) {
        return Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE");
    }
    return ScanAndAdmitExpectedModules(context, false);
}

template <typename Original, typename Path, typename Call>
HMODULE HookLoader(
    const Original original,
    const Path path,
    Call&& call) noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || original == nullptr || IsFatal(context)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    CallbackLease callback;
    if (IsFatal(context)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (IsWithinLoaderCallout(context)) {
        static_cast<void>(Fatal(context, "STEAM_LOADER_CALLOUT_LOAD_DENIED"));
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const bool requestedTarget = RequestedTarget(context, path);
    if (callback.IsNested() && internalBypassDepth == 0) {
        static_cast<void>(Fatal(context, "STEAM_NESTED_LOAD_DENIED"));
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    UniqueHandle preflightFile;
    if (internalBypassDepth == 0
        && !PreflightLoad(context, path, requestedTarget, preflightFile)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    OneShotNativeDelegation delegation(nativeLoadDelegationAllowance);
    const HMODULE module = call();
    if (internalBypassDepth != 0) {
        if (IsFatal(context)) {
            Quarantine(context, module);
            return nullptr;
        }
        return module;
    }
    if (IsFatal(context)) {
        Quarantine(context, module);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (module == nullptr || callback.IsNested()) {
        return IsFatal(context) ? nullptr : module;
    }
    if (!AdmitAfterLoad(context, module, requestedTarget) || IsFatal(context)) {
        Quarantine(context, module);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    return module;
}

HMODULE WINAPI HookLoadLibraryA(const LPCSTR fileName) {
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto original = context == nullptr
        ? nullptr
        : context->trampolines.loadLibraryA;
    return HookLoader(original, fileName, [original, fileName]() {
        return original(fileName);
    });
}

HMODULE WINAPI HookLoadLibraryW(const LPCWSTR fileName) {
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto original = context == nullptr
        ? nullptr
        : context->trampolines.loadLibraryW;
    return HookLoader(
        original,
        std::wstring_view(fileName == nullptr ? L"" : fileName),
        [original, fileName]() { return original(fileName); });
}

HMODULE WINAPI HookLoadLibraryExA(
    const LPCSTR fileName,
    const HANDLE file,
    const DWORD flags) {
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto original = context == nullptr
        ? nullptr
        : context->trampolines.loadLibraryExA;
    return HookLoader(original, fileName, [original, fileName, file, flags]() {
        return original(fileName, file, flags);
    });
}

HMODULE WINAPI HookLoadLibraryExW(
    const LPCWSTR fileName,
    const HANDLE file,
    const DWORD flags) {
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto original = context == nullptr
        ? nullptr
        : context->trampolines.loadLibraryExW;
    return HookLoader(
        original,
        std::wstring_view(fileName == nullptr ? L"" : fileName),
        [original, fileName, file, flags]() {
            return original(fileName, file, flags);
        });
}

FARPROC WINAPI HookGetProcAddress(
    const HMODULE module,
    const LPCSTR procedureName) {
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.getProcAddress == nullptr
        || IsFatal(context)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (internalBypassDepth != 0) {
        const auto result = CallOriginalGetProcAddress(
            context,
            module,
            procedureName);
        if (IsFatal(context)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        return result;
    }
    CallbackLease callback;
    if (IsFatal(context)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (IsWithinLoaderCallout(context)) {
        static_cast<void>(Fatal(
            context,
            "STEAM_LOADER_CALLOUT_SYMBOL_DENIED"));
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (callback.IsNested()) {
        if (ModuleIsRequestedTarget(context, module)) {
            static_cast<void>(Fatal(context, "STEAM_NESTED_PROTECTED_SYMBOL"));
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        const auto result = CallOriginalGetProcAddress(
            context,
            module,
            procedureName);
        return IsFatal(context) ? nullptr : result;
    }

    const auto admission = AdmitModule(context, module);
    if (!admission.target) {
        const auto result = CallOriginalGetProcAddress(
            context,
            module,
            procedureName);
        return IsFatal(context) ? nullptr : result;
    }
    if (!admission.admitted || admission.record == nullptr || IsFatal(context)) {
        return nullptr;
    }
    if (reinterpret_cast<std::uintptr_t>(procedureName) <= 0xffffU) {
        static_cast<void>(Fatal(context, "STEAM_SYMBOL_UNSUPPORTED"));
        return nullptr;
    }
    const std::string_view requested(procedureName);
    const auto found = std::find_if(
        admission.record->protectedExports.begin(),
        admission.record->protectedExports.end(),
        [requested](const ProtectedExport& exportEntry) {
            return exportEntry.name == requested;
        });
    if (found != admission.record->protectedExports.end()) {
        return IsFatal(context)
            ? nullptr
            : reinterpret_cast<FARPROC>(
                Steam::SteamFactoryDetourAddress(found->slot));
    }
    const auto result = CallOriginalGetProcAddress(
        context,
        module,
        procedureName);
    return IsFatal(context) ? nullptr : result;
}

NTSTATUS NTAPI HookLdrLoadDll(
    PWSTR const searchPath,
    PULONG const dllCharacteristics,
    PUNICODE_STRING const moduleFileName,
    PHANDLE const moduleHandle) noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    if (!SafeWritePointer(
            reinterpret_cast<void**>(moduleHandle),
            nullptr)) {
        static_cast<void>(Fatal(context, "STEAM_NATIVE_LOAD_MALFORMED"));
        return kNativeAccessDenied;
    }
    if (context == nullptr || context->trampolines.ldrLoadDll == nullptr
        || IsFatal(context)) {
        return kNativeAccessDenied;
    }

    HMODULE loaded = nullptr;
    if (nativeLoadDelegationAllowance != 0
        && !IsWithinLoaderCallout(context)
        && ConsumeNativeDelegation(nativeLoadDelegationAllowance)) {
        const auto status = context->trampolines.ldrLoadDll(
            searchPath,
            dllCharacteristics,
            moduleFileName,
            reinterpret_cast<PHANDLE>(&loaded));
        if (IsFatal(context)) {
            Quarantine(context, loaded);
            return kNativeAccessDenied;
        }
        if (status >= kNativeSuccess
            && !SafeWritePointer(
                reinterpret_cast<void**>(moduleHandle),
                loaded)) {
            Quarantine(context, loaded);
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_LOAD_MALFORMED"));
            return kNativeAccessDenied;
        }
        return status;
    }

    CallbackLease callback;
    if (IsFatal(context)) {
        return kNativeAccessDenied;
    }
    if (IsWithinLoaderCallout(context)) {
        static_cast<void>(Fatal(
            context,
            "STEAM_LOADER_CALLOUT_LOAD_DENIED"));
        return kNativeAccessDenied;
    }
    if (callback.IsNested() && internalBypassDepth == 0) {
        static_cast<void>(Fatal(context, "STEAM_NESTED_LOAD_DENIED"));
        return kNativeAccessDenied;
    }

    std::wstring requestedPath;
    if (!CopyNativePath(moduleFileName, requestedPath)) {
        static_cast<void>(Fatal(context, "STEAM_NATIVE_LOAD_MALFORMED"));
        return kNativeAccessDenied;
    }
    if (searchPath != nullptr || dllCharacteristics != nullptr
        || !IsCanonicalAbsolutePath(requestedPath)) {
        static_cast<void>(Fatal(
            context,
            "STEAM_NATIVE_LOAD_PATH_AMBIGUOUS"));
        return kNativeAccessDenied;
    }

    const bool requestedTarget = RequestedTarget(context, requestedPath);
    UniqueHandle preflightFile;
    if (internalBypassDepth == 0
        && !PreflightLoad(
            context,
            std::wstring_view(requestedPath),
            requestedTarget,
            preflightFile)) {
        return kNativeAccessDenied;
    }

    UNICODE_STRING stableName{};
    stableName.Buffer = requestedPath.data();
    stableName.Length = static_cast<USHORT>(
        requestedPath.size() * sizeof(wchar_t));
    stableName.MaximumLength = stableName.Length;
    const auto status = context->trampolines.ldrLoadDll(
        nullptr,
        nullptr,
        &stableName,
        reinterpret_cast<PHANDLE>(&loaded));
    if (IsFatal(context)) {
        Quarantine(context, loaded);
        return kNativeAccessDenied;
    }
    if (status < kNativeSuccess || loaded == nullptr) {
        return status;
    }
    if (!AdmitAfterLoad(context, loaded, requestedTarget) || IsFatal(context)) {
        Quarantine(context, loaded);
        return kNativeAccessDenied;
    }
    if (!SafeWritePointer(
            reinterpret_cast<void**>(moduleHandle),
            loaded)) {
        Quarantine(context, loaded);
        static_cast<void>(Fatal(context, "STEAM_NATIVE_LOAD_MALFORMED"));
        return kNativeAccessDenied;
    }
    return status;
}

template <typename Original>
NTSTATUS GuardNativeProcedureAddress(
    const std::shared_ptr<GateContext>& context,
    PVOID const module,
    PANSI_STRING const procedureName,
    const ULONG ordinal,
    PVOID* const procedureAddress,
    Original&& original) noexcept {
    if (!SafeWritePointer(procedureAddress, nullptr)) {
        static_cast<void>(Fatal(context, "STEAM_NATIVE_SYMBOL_MALFORMED"));
        return kNativeAccessDenied;
    }
    if (context == nullptr || IsFatal(context)) {
        return kNativeAccessDenied;
    }

    void* result = nullptr;
    const auto invokeOriginal = [&original](
        PANSI_STRING const name,
        const ULONG number,
        void** const destination) noexcept {
        OneShotNativeDelegation delegation(nativeSymbolDelegationAllowance);
        return original(name, number, destination);
    };
    if (internalBypassDepth != 0) {
        const auto status = invokeOriginal(
            procedureName,
            ordinal,
            &result);
        if (IsFatal(context)) {
            return kNativeAccessDenied;
        }
        if (status >= kNativeSuccess
            && !SafeWritePointer(procedureAddress, result)) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_SYMBOL_MALFORMED"));
            return kNativeAccessDenied;
        }
        return status;
    }
    if (nativeSymbolDelegationAllowance != 0
        && !IsWithinLoaderCallout(context)
        && ConsumeNativeDelegation(nativeSymbolDelegationAllowance)) {
        const auto status = original(
            procedureName,
            ordinal,
            &result);
        if (IsFatal(context)) {
            return kNativeAccessDenied;
        }
        if (status >= kNativeSuccess
            && !SafeWritePointer(procedureAddress, result)) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_SYMBOL_MALFORMED"));
            return kNativeAccessDenied;
        }
        return status;
    }

    CallbackLease callback;
    if (IsFatal(context)) {
        return kNativeAccessDenied;
    }
    if (IsWithinLoaderCallout(context)) {
        static_cast<void>(Fatal(
            context,
            "STEAM_LOADER_CALLOUT_SYMBOL_DENIED"));
        return kNativeAccessDenied;
    }

    const bool byOrdinal = procedureName == nullptr && ordinal != 0;
    std::string requestedName;
    if ((!byOrdinal && ordinal != 0)
        || (procedureName == nullptr && ordinal == 0)
        || (procedureName != nullptr
            && !CopyNativeProcedureName(procedureName, requestedName))) {
        static_cast<void>(Fatal(
            context,
            "STEAM_NATIVE_SYMBOL_MALFORMED"));
        return kNativeAccessDenied;
    }
    ANSI_STRING stableProcedureName{};
    PANSI_STRING forwardedName = nullptr;
    if (!byOrdinal) {
        stableProcedureName.Buffer = requestedName.data();
        stableProcedureName.Length = static_cast<USHORT>(
            requestedName.size());
        stableProcedureName.MaximumLength = stableProcedureName.Length;
        forwardedName = &stableProcedureName;
    }

    if (callback.IsNested()) {
        if (ModuleIsRequestedTarget(
                context,
                static_cast<HMODULE>(module))) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NESTED_PROTECTED_SYMBOL"));
            return kNativeAccessDenied;
        }
        const auto status = invokeOriginal(
            forwardedName,
            ordinal,
            &result);
        if (IsFatal(context)) {
            return kNativeAccessDenied;
        }
        if (status >= kNativeSuccess
            && !SafeWritePointer(procedureAddress, result)) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_SYMBOL_MALFORMED"));
            return kNativeAccessDenied;
        }
        return status;
    }

    const auto admission = AdmitModule(
        context,
        static_cast<HMODULE>(module));
    if (!admission.target) {
        const auto status = invokeOriginal(
            forwardedName,
            ordinal,
            &result);
        if (IsFatal(context)) {
            return kNativeAccessDenied;
        }
        if (status >= kNativeSuccess
            && !SafeWritePointer(procedureAddress, result)) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_SYMBOL_MALFORMED"));
            return kNativeAccessDenied;
        }
        return status;
    }
    if (!admission.admitted || admission.record == nullptr
        || IsFatal(context)) {
        return kNativeAccessDenied;
    }
    if (byOrdinal) {
        static_cast<void>(Fatal(context, "STEAM_SYMBOL_UNSUPPORTED"));
        return kNativeAccessDenied;
    }

    const auto found = std::find_if(
        admission.record->protectedExports.begin(),
        admission.record->protectedExports.end(),
        [&requestedName](const ProtectedExport& exportEntry) {
            return exportEntry.name == requestedName;
        });
    if (found != admission.record->protectedExports.end()) {
        result = Steam::SteamFactoryDetourAddress(found->slot);
        if (result == nullptr || IsFatal(context)
            || !SafeWritePointer(procedureAddress, result)) {
            static_cast<void>(Fatal(
                context,
                "STEAM_NATIVE_SYMBOL_MALFORMED"));
            return kNativeAccessDenied;
        }
        return kNativeSuccess;
    }

    const auto status = invokeOriginal(
        forwardedName,
        ordinal,
        &result);
    if (IsFatal(context)) {
        return kNativeAccessDenied;
    }
    if (status >= kNativeSuccess
        && !SafeWritePointer(procedureAddress, result)) {
        static_cast<void>(Fatal(
            context,
            "STEAM_NATIVE_SYMBOL_MALFORMED"));
        return kNativeAccessDenied;
    }
    return status;
}

NTSTATUS NTAPI HookLdrGetProcedureAddress(
    PVOID const module,
    PANSI_STRING const procedureName,
    const ULONG ordinal,
    PVOID* const procedureAddress) noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    return GuardNativeProcedureAddress(
        context,
        module,
        procedureName,
        ordinal,
        procedureAddress,
        [&context, module](
            PANSI_STRING const forwardedName,
            const ULONG forwardedOrdinal,
            PVOID* const result) noexcept {
            return context == nullptr
                    || context->trampolines.ldrGetProcedureAddress == nullptr
                ? kNativeAccessDenied
                : context->trampolines.ldrGetProcedureAddress(
                    module,
                    forwardedName,
                    forwardedOrdinal,
                    result);
        });
}

NTSTATUS NTAPI HookLdrGetProcedureAddressEx(
    PVOID const module,
    PANSI_STRING const procedureName,
    const ULONG ordinal,
    PVOID* const procedureAddress,
    const ULONG flags) noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    return GuardNativeProcedureAddress(
        context,
        module,
        procedureName,
        ordinal,
        procedureAddress,
        [&context, module, flags](
            PANSI_STRING const forwardedName,
            const ULONG forwardedOrdinal,
            PVOID* const result) noexcept {
            return context == nullptr
                    || context->trampolines.ldrGetProcedureAddressEx == nullptr
                ? kNativeAccessDenied
                : context->trampolines.ldrGetProcedureAddressEx(
                    module,
                    forwardedName,
                    forwardedOrdinal,
                    result,
                    flags);
        });
}

NTSTATUS NTAPI HookLdrGetProcedureAddressForCaller(
    PVOID const module,
    PANSI_STRING const procedureName,
    const ULONG ordinal,
    PVOID* const procedureAddress,
    const ULONG flags,
    PVOID const callerAddress) noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    return GuardNativeProcedureAddress(
        context,
        module,
        procedureName,
        ordinal,
        procedureAddress,
        [&context,
         module,
         flags,
         callerAddress](
            PANSI_STRING const forwardedName,
            const ULONG forwardedOrdinal,
            PVOID* const result) noexcept {
            return context == nullptr
                    || context->trampolines.
                        ldrGetProcedureAddressForCaller == nullptr
                ? kNativeAccessDenied
                : context->trampolines.ldrGetProcedureAddressForCaller(
                    module,
                    forwardedName,
                    forwardedOrdinal,
                    result,
                    flags,
                    callerAddress);
        });
}

struct LoaderHookDefinition {
    HMODULE module;
    const char* procedure;
    void* detour;
    void** original;
};

std::vector<LoaderHookDefinition> LoaderHooks(
    GateContext& context,
    const HMODULE kernel,
    const HMODULE ntdll) {
    std::vector<LoaderHookDefinition> definitions{{
        {kernel, "LoadLibraryA", reinterpret_cast<void*>(&HookLoadLibraryA),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryA)},
        {kernel, "LoadLibraryW", reinterpret_cast<void*>(&HookLoadLibraryW),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryW)},
        {kernel, "LoadLibraryExA", reinterpret_cast<void*>(&HookLoadLibraryExA),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryExA)},
        {kernel, "LoadLibraryExW", reinterpret_cast<void*>(&HookLoadLibraryExW),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryExW)},
        {kernel, "GetProcAddress", reinterpret_cast<void*>(&HookGetProcAddress),
         reinterpret_cast<void**>(&context.trampolines.getProcAddress)},
        {ntdll, "LdrLoadDll", reinterpret_cast<void*>(&HookLdrLoadDll),
         reinterpret_cast<void**>(&context.trampolines.ldrLoadDll)},
        {ntdll, "LdrGetProcedureAddress",
         reinterpret_cast<void*>(&HookLdrGetProcedureAddress),
         reinterpret_cast<void**>(
             &context.trampolines.ldrGetProcedureAddress)},
    }};
    definitions.reserve(9);
    if (GetProcAddress(ntdll, "LdrGetProcedureAddressEx") != nullptr) {
        definitions.push_back({
            ntdll,
            "LdrGetProcedureAddressEx",
            reinterpret_cast<void*>(&HookLdrGetProcedureAddressEx),
            reinterpret_cast<void**>(
                &context.trampolines.ldrGetProcedureAddressEx),
        });
    }
    if (GetProcAddress(ntdll, "LdrGetProcedureAddressForCaller") != nullptr) {
        definitions.push_back({
            ntdll,
            "LdrGetProcedureAddressForCaller",
            reinterpret_cast<void*>(&HookLdrGetProcedureAddressForCaller),
            reinterpret_cast<void**>(
                &context.trampolines.ldrGetProcedureAddressForCaller),
        });
    }
    return definitions;
}

bool BuildContext(
    const DeferredModuleGateConfiguration& configuration,
    std::shared_ptr<GateContext>& context) {
    if (configuration.modules.empty()
        || configuration.modules.size() > Steam::kSteamFactorySlotCapacity
        || configuration.fatalReporter == nullptr) {
        return false;
    }
    auto candidate = std::make_shared<GateContext>();
    candidate->identityLease = configuration.identityLease;
    candidate->fatalState = std::make_shared<Steam::FatalState>(
        configuration.fatalReporter);
    const Steam::SteamPolicy policy;
    std::size_t exportCount = 0;
    for (const auto& source : configuration.modules) {
        if (!IsCanonicalAbsolutePath(source.expectedPath)
            || source.declaredInterfaces.empty()
            || source.protectedFactoryExports.empty()) {
            return false;
        }
        exportCount += source.protectedFactoryExports.size();
        if (exportCount > Steam::kSteamFactorySlotCapacity) {
            return false;
        }
        auto record = std::make_unique<ModuleRecord>();
        record->expectedFile = OpenPinnedReadOnly(source.expectedPath);
        if (record->expectedFile.value == INVALID_HANDLE_VALUE
            || !ReadCanonicalPath(
                record->expectedFile.value,
                record->canonicalPath)
            || !ReadFileIdentity(
                record->expectedFile.value,
                record->expectedIdentity)) {
            return false;
        }
        const std::wstring_view finalPath(record->canonicalPath);
        if (!finalPath.starts_with(L"\\\\?\\")
            || !EqualsPath(source.expectedPath, finalPath.substr(4))) {
            return false;
        }
        record->baseName = BaseName(record->canonicalPath);
        record->expectedSha256 = source.expectedSha256;
        record->allowDeferred = source.allowDeferred;
        record->declaredInterfaces = source.declaredInterfaces;
        for (const auto& version : record->declaredInterfaces) {
            const auto decision = policy.EvaluateInterface(version);
            if (version.empty()
                || decision == Steam::InterfaceDecision::UnknownProtectedFatal
                || decision == Steam::InterfaceDecision::Unrecognized) {
                return false;
            }
        }
        if (std::any_of(
                record->declaredInterfaces.begin(),
                record->declaredInterfaces.end(),
                [&record](const std::string& version) {
                    return std::count(
                        record->declaredInterfaces.begin(),
                        record->declaredInterfaces.end(),
                        version) != 1;
                })) {
            return false;
        }
        for (const auto& name : source.protectedFactoryExports) {
            if (name.empty()
                || std::any_of(
                    name.begin(),
                    name.end(),
                    [](const char character) {
                        return std::isspace(
                            static_cast<unsigned char>(character)) != 0;
                })) {
                return false;
            }
            record->protectedExports.push_back({name});
        }
        if (std::any_of(
                record->protectedExports.begin(),
                record->protectedExports.end(),
                [&record](const ProtectedExport& exportEntry) {
                    return std::count_if(
                        record->protectedExports.begin(),
                        record->protectedExports.end(),
                        [&exportEntry](const ProtectedExport& candidateExport) {
                            return candidateExport.name == exportEntry.name;
                        }) != 1;
                })) {
            return false;
        }
        const auto duplicate = std::find_if(
            candidate->modules.begin(),
            candidate->modules.end(),
            [&record](const std::unique_ptr<ModuleRecord>& existing) {
                return existing->baseName == record->baseName
                    || EqualsPath(existing->canonicalPath, record->canonicalPath);
            });
        if (duplicate != candidate->modules.end()) {
            return false;
        }
        candidate->modules.push_back(std::move(record));
    }
    context = std::move(candidate);
    return true;
}

DeferredModuleGateCleanupStatus CleanupLocked() noexcept {
    gateInstalled.store(false, std::memory_order_release);
    if (lifecycle.context != nullptr && lifecycle.context->fatalState != nullptr) {
        lifecycle.context->fatalState->EnterDenyOnly();
    }
    auto disableKnownHooks = []() noexcept {
        bool disabled = true;
        std::scoped_lock hookLock(hooksMutex);
        for (auto& hook : lifecycle.hooks) {
            if (!hook.created || !hook.enabled) {
                continue;
            }
            const auto status = Hooks::DisableHook(hook.target);
            const bool current = status == MH_OK || status == MH_ERROR_DISABLED
                || status == MH_ERROR_NOT_CREATED;
            if (current) {
                hook.enabled = false;
            }
            disabled = current && disabled;
        }
        return disabled;
    };
    {
        Hooks::MinHookMutationLease mutation;
        if (!disableKnownHooks()) {
            static_cast<void>(EnsureRetainedHooksEnabled());
            return DeferredModuleGateCleanupStatus::Incomplete;
        }
    }
    SignalTestEvent(afterInitialDisableEvent);

    {
        std::unique_lock callbackDrain(callbackGate);
    }
    SignalTestEvent(beforeFactoryDrainEvent);
    Steam::SteamFactoryCallbackBlock factoryCallbackBlock;
    std::unique_lock callbackLock(callbackGate);
    {
        Hooks::MinHookMutationLease mutation;
        if (!disableKnownHooks()) {
            static_cast<void>(EnsureRetainedHooksEnabled());
            return DeferredModuleGateCleanupStatus::Incomplete;
        }
        bool removed = true;
        bool removedAny = false;
        {
            std::scoped_lock hookLock(hooksMutex);
            for (auto iterator = lifecycle.hooks.rbegin();
                 iterator != lifecycle.hooks.rend();
                 ++iterator) {
                if (!iterator->created) {
                    continue;
                }
                const auto status = Hooks::RemoveHook(iterator->target);
                if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
                    iterator->created = false;
                    removedAny = true;
                    if (iterator->factorySlot
                        < Steam::kSteamFactorySlotCapacity) {
                        Steam::UnregisterSteamFactorySlot(
                            iterator->factorySlot);
                        if (lifecycle.context != nullptr) {
                            ReleaseFactorySlot(
                                *lifecycle.context,
                                iterator->factorySlot);
                        }
                    }
                }
                else {
                    removed = false;
                }
            }
        }
        if (!removed) {
            if (removedAny) {
                lifecycle.coverageComplete = false;
            }
            static_cast<void>(EnsureRetainedHooksEnabled());
            return DeferredModuleGateCleanupStatus::Incomplete;
        }
        if (lifecycle.initialized && !Hooks::ReleaseMinHook()) {
            return DeferredModuleGateCleanupStatus::Incomplete;
        }
        lifecycle.initialized = false;
        activeContext.store({}, std::memory_order_release);
        lifecycle = {};
    }
    return DeferredModuleGateCleanupStatus::Success;
}

DeferredModuleGateInstallStatus InstallConfiguredGate(
    const DeferredModuleGateConfiguration& configuration,
    const Steam::SteamInterfaceLayout interfaceLayout) noexcept {
    std::scoped_lock installLock(installMutex);
    if (lifecycle.context != nullptr
        || activeContext.load(std::memory_order_acquire) != nullptr) {
        return DeferredModuleGateInstallStatus::HookInstallFailed;
    }
    try {
        Hooks::MinHookMutationLease mutation;
        std::shared_ptr<GateContext> context;
        if (!BuildContext(configuration, context)) {
            return DeferredModuleGateInstallStatus::InvalidConfiguration;
        }
        context->interfaceLayout = interfaceLayout;
        if (!Hooks::AcquireMinHook()) {
            return DeferredModuleGateInstallStatus::HookInstallFailed;
        }
        lifecycle.initialized = true;
        lifecycle.context = context;
        bool ready = true;
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (kernel == nullptr || ntdll == nullptr) {
            ready = false;
        }
        if (ready) {
            context->loaderCalloutProbe =
                reinterpret_cast<LoaderCalloutProbeFunction>(
                    GetProcAddress(ntdll, "RtlIsThreadWithinLoaderCallout"));
            ready = context->loaderCalloutProbe != nullptr;
        }
        if (ready) {
            const auto definitions = LoaderHooks(*context, kernel, ntdll);
            lifecycle.hooks.reserve(
                definitions.size() + Steam::kSteamFactorySlotCapacity);
            for (const auto& definition : definitions) {
                void* const target = reinterpret_cast<void*>(
                    GetProcAddress(
                        definition.module,
                        definition.procedure));
                if (target == nullptr
                    || Hooks::CreateHook(
                        target,
                        definition.detour,
                        definition.original) != MH_OK) {
                    ready = false;
                    break;
                }
                lifecycle.hooks.push_back({target, true, false});
            }
        }
        if (ready) {
            for (const auto& hook : lifecycle.hooks) {
                if (Hooks::QueueEnableHook(hook.target) != MH_OK) {
                    ready = false;
                    break;
                }
            }
        }
        if (ready) {
            activeContext.store(context, std::memory_order_release);
            ready = Hooks::ApplyQueuedHooks() == MH_OK;
            if (ready) {
                std::scoped_lock hookLock(hooksMutex);
                for (auto& hook : lifecycle.hooks) {
                    if (hook.created) {
                        hook.enabled = true;
                    }
                }
                lifecycle.coverageComplete = true;
            }
        }
        mutation.Release();
        if (!ready) {
            static_cast<void>(Fatal(context, "STEAM_HOOK_FAILED"));
            static_cast<void>(CleanupLocked());
            return DeferredModuleGateInstallStatus::HookInstallFailed;
        }

        // Toolhelp is not independently loader-stable. The caller must invoke
        // initial installation before the target is resumed; this function does
        // not itself prove suspension. After resume, all scans run from the
        // serialized outermost loader callback.
        if (!ScanAndAdmitExpectedModules(context, false)) {
            if (!LifecycleOwnsFactoryHooks()) {
                static_cast<void>(CleanupLocked());
            }
            return DeferredModuleGateInstallStatus::AdmissionFailed;
        }
        gateInstalled.store(true, std::memory_order_release);
        return DeferredModuleGateInstallStatus::Success;
    }
    catch (...) {
        if (lifecycle.context != nullptr) {
            static_cast<void>(Fatal(lifecycle.context, "STEAM_GATE_UNAVAILABLE"));
        }
        if (!LifecycleOwnsFactoryHooks()) {
            static_cast<void>(CleanupLocked());
        }
        return DeferredModuleGateInstallStatus::HookInstallFailed;
    }
}

}  // namespace

DeferredModuleGateInstallStatus InstallDeferredModuleGate(
    const DeferredModuleGateConfiguration& configuration) noexcept {
    return InstallConfiguredGate(
        configuration,
        Steam::SteamInterfaceLayout::ProductionPinned);
}

DeferredModuleGateCleanupStatus UninstallDeferredModuleGate() noexcept {
    std::scoped_lock installLock(installMutex);
    return CleanupLocked();
}

bool DeferredModuleGateIsInstalled() noexcept {
    return gateInstalled.load(std::memory_order_acquire);
}

namespace Testing {

DeferredModuleGateInstallStatus
InstallDeferredModuleGateForSyntheticSuspendedProcess(
    const DeferredModuleGateConfiguration& configuration) noexcept {
    return InstallConfiguredGate(
        configuration,
        Steam::SteamInterfaceLayout::SyntheticOneSlotForTesting);
}

void FailNextFactoryPublication() noexcept {
    failFactoryPublication.fetch_add(1, std::memory_order_acq_rel);
}

void FailNextFactoryPostCreateBookkeeping() noexcept {
    failFactoryPostCreate.fetch_add(1, std::memory_order_acq_rel);
}

DeferredModuleGateLifecycleSnapshot CurrentGateLifecycle() noexcept {
    std::scoped_lock installLock(installMutex);
    DeferredModuleGateLifecycleSnapshot snapshot{};
    snapshot.contextRetained = lifecycle.context != nullptr;
    std::scoped_lock hookLock(hooksMutex);
    snapshot.denyOnly = lifecycle.context != nullptr
        && lifecycle.context->fatalState != nullptr
        && lifecycle.context->fatalState->IsFatal()
        && lifecycle.coverageComplete
        && AllCreatedHooksEnabledLocked();
    for (const auto& hook : lifecycle.hooks) {
        if (!hook.created) {
            continue;
        }
        ++snapshot.hooksRetained;
        if (hook.factorySlot < Steam::kSteamFactorySlotCapacity) {
            ++snapshot.factorySlotsRetained;
        }
    }
    return snapshot;
}

void SetGateCleanupPhaseEvents(
    void* const afterDisable,
    void* const beforeFactory) noexcept {
    afterInitialDisableEvent.store(
        static_cast<HANDLE>(afterDisable),
        std::memory_order_release);
    beforeFactoryDrainEvent.store(
        static_cast<HANDLE>(beforeFactory),
        std::memory_order_release);
}

void SetFactoryPublicationPauseEvents(
    void* const afterApply,
    void* const allowRollback) noexcept {
    afterFactoryApplyEvent.store(
        static_cast<HANDLE>(afterApply),
        std::memory_order_release);
    allowFactoryRollbackEvent.store(
        static_cast<HANDLE>(allowRollback),
        std::memory_order_release);
}

void HoldGateCallbackWhileWaitingForMutation(
    void* const entered,
    void* const allowMutation,
    void* const mutationAcquired,
    void* const release) noexcept {
    CallbackLease callback;
    if (entered != nullptr) {
        SetEvent(static_cast<HANDLE>(entered));
    }
    if (allowMutation != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(allowMutation), INFINITE);
    }
    Hooks::MinHookMutationLease mutation;
    if (mutationAcquired != nullptr) {
        SetEvent(static_cast<HANDLE>(mutationAcquired));
    }
    if (release != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(release), INFINITE);
    }
}

bool GateIsDenyOnlyForReporter() noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->fatalState == nullptr
        || !context->fatalState->IsFatal()) {
        return false;
    }
    std::scoped_lock hookLock(hooksMutex);
    return lifecycle.coverageComplete && AllCreatedHooksEnabledLocked();
}

std::size_t GateRetainedFactorySlotCountForReporter() noexcept {
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr) {
        return 0;
    }
    std::scoped_lock lock(context->slotMutex);
    return static_cast<std::size_t>(std::count(
        context->slots.begin(),
        context->slots.end(),
        true));
}

std::int32_t CallOriginalLdrLoadDllForSyntheticCallout(
    const std::wstring& path,
    void** const module) noexcept {
    if (module == nullptr || path.empty()
        || path.size() > USHRT_MAX / sizeof(wchar_t)) {
        return static_cast<std::int32_t>(0xc000000dU);
    }
    *module = nullptr;
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto load = context == nullptr
        ? nullptr
        : context->trampolines.ldrLoadDll;
    if (load == nullptr) {
        return static_cast<std::int32_t>(0xc0000139U);
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(path.data());
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    HANDLE loaded = nullptr;
    const auto status = load(nullptr, nullptr, &name, &loaded);
    *module = loaded;
    return status;
}

NativeSymbolDelegationProbeSnapshot
ProbeNativeSymbolDelegationChain() noexcept {
    NativeSymbolDelegationProbeSnapshot snapshot{};
    const auto context = activeContext.load(std::memory_order_acquire);
    const auto module = GetModuleHandleW(L"kernel32.dll");
    if (context == nullptr || module == nullptr) {
        snapshot.status = kNativeAccessDenied;
        return snapshot;
    }

    char firstNameBuffer[] = "first";
    ANSI_STRING firstName{};
    firstName.Buffer = firstNameBuffer;
    firstName.Length = 5;
    firstName.MaximumLength = 5;
    char chainedNameBuffer[] = "x";
    ANSI_STRING malformedChainedName{};
    malformedChainedName.Buffer = chainedNameBuffer;
    malformedChainedName.Length = 1;
    malformedChainedName.MaximumLength = 0;
    void* result = reinterpret_cast<void*>(1);
    nativeSymbolDelegationAllowance = 1;
    snapshot.status = GuardNativeProcedureAddress(
        context,
        module,
        &firstName,
        0,
        &result,
        [&context, module, &snapshot, &malformedChainedName](
            PANSI_STRING,
            const ULONG,
            void** const destination) noexcept {
            ++snapshot.firstOriginalCalls;
            void* chainedResult = reinterpret_cast<void*>(1);
            const auto chainedStatus = GuardNativeProcedureAddress(
                context,
                module,
                &malformedChainedName,
                0,
                &chainedResult,
                [&snapshot](
                    PANSI_STRING,
                    const ULONG,
                    void** const chainedDestination) noexcept {
                    ++snapshot.chainedOriginalCalls;
                    *chainedDestination = reinterpret_cast<void*>(2);
                    return kNativeSuccess;
                });
            if (chainedStatus >= kNativeSuccess) {
                *destination = chainedResult;
            }
            return chainedStatus;
        });
    nativeSymbolDelegationAllowance = 0;
    snapshot.resultPublished = result != nullptr;
    return snapshot;
}

void SetCountedStringSnapshotHook(
    const CountedStringSnapshotHook hook) noexcept {
    countedStringSnapshotHook.store(hook, std::memory_order_release);
}

}  // namespace Testing

}  // namespace DSRRandomizer::Modules

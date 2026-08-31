#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using ModEngineInit = bool (*)(void*, void**);

namespace {

constexpr std::size_t kFingerprintSize = 14;

struct FixtureCallsite final {
    std::byte* address = nullptr;
    std::array<std::uint8_t, kFingerprintSize> expected{};
    std::size_t pathArgumentIndex = 0;
};

void** FindCreateFileImportSlot() noexcept {
    const auto* const base = reinterpret_cast<const std::byte*>(
        GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return nullptr;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE
        || headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    const auto& imports = headers->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0 || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return nullptr;
    }
    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk != 0
                        ? descriptor->OriginalFirstThunk
                        : descriptor->FirstThunk));
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::byte*>(base) + descriptor->FirstThunk);
        for (; lookup->u1.AddressOfData != 0; ++lookup, ++address) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
                continue;
            }
            const auto* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + lookup->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), "CreateFileW")
                == 0) {
                return reinterpret_cast<void**>(&address->u1.Function);
            }
        }
    }
    return nullptr;
}

__declspec(noinline) HANDLE OpenFixtureSaveForWrite(const wchar_t* path) {
    const auto handle = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD flags{};
        static_cast<void>(GetHandleInformation(handle, &flags));
    }
    return handle;
}

__declspec(noinline) HANDLE OpenFixtureSaveForRead(const wchar_t* path) {
    const auto handle = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD flags{};
        static_cast<void>(GetHandleInformation(handle, &flags));
    }
    return handle;
}

bool DescribeExecutedCallsite(
    const void* const function,
    void** const importSlot,
    FixtureCallsite& target) noexcept {
    constexpr std::size_t scanLength = 256;
    const auto* const bytes = reinterpret_cast<const std::byte*>(function);
    if (bytes == nullptr || importSlot == nullptr) {
        return false;
    }
    for (std::size_t offset = 0; offset + 6 <= scanLength; ++offset) {
        if (bytes[offset] != std::byte{0xff} || bytes[offset + 1] != std::byte{0x15}) {
            continue;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, bytes + offset + 2, sizeof(displacement));
        const auto* const resolvedSlot = reinterpret_cast<void* const*>(
            reinterpret_cast<std::intptr_t>(bytes + offset + 6) + displacement);
        if (resolvedSlot != importSlot) {
            continue;
        }
        const auto start = offset >= 2 ? offset - 2 : offset;
        if (start + kFingerprintSize > scanLength) {
            return false;
        }
        target.address = const_cast<std::byte*>(bytes + start);
        std::memcpy(target.expected.data(), bytes + start, target.expected.size());
        target.pathArgumentIndex = offset - start;
        return true;
    }
    return false;
}

bool BuildCallsites(FixtureCallsite* const targets) noexcept {
    auto** const importSlot = FindCreateFileImportSlot();
    return DescribeExecutedCallsite(
               reinterpret_cast<const void*>(&OpenFixtureSaveForWrite),
               importSlot,
               targets[0])
        && DescribeExecutedCallsite(
               reinterpret_cast<const void*>(&OpenFixtureSaveForRead),
               importSlot,
               targets[1]);
}

}  // namespace

extern "C" __declspec(dllexport) bool DsrGetRmmBridgeIntegrationSaveCallsites(
    FixtureCallsite* const targets,
    const std::size_t count) noexcept {
    return targets != nullptr && count == 2 && BuildCallsites(targets);
}

std::wstring Join(std::wstring left, const std::wstring& right) {
    left.push_back(L'\\');
    left.append(right);
    return left;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        return 2;
    }
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

    const auto save = OpenFixtureSaveForWrite(argv[2]);
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

    const auto redirected = OpenFixtureSaveForRead(argv[2]);
    if (redirected == INVALID_HANDLE_VALUE) {
        return 11;
    }
    CloseHandle(redirected);
    return 0;
}

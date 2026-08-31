#include <Windows.h>
#include <knownfolders.h>
#include <shlobj.h>

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

std::array<std::array<std::byte, kFingerprintSize>, 2> callsiteBytes{};

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

bool PopulateCallsite(
    std::array<std::byte, kFingerprintSize>& bytes,
    const std::size_t callOffset,
    void** const importSlot,
    const std::array<std::uint8_t, kFingerprintSize>& suffix) noexcept {
    if (importSlot == nullptr || callOffset > kFingerprintSize - 6) {
        return false;
    }
    bytes = {};
    std::memcpy(bytes.data(), suffix.data(), suffix.size());
    bytes[callOffset] = std::byte{0xff};
    bytes[callOffset + 1] = std::byte{0x15};
    const auto displacement = reinterpret_cast<std::intptr_t>(importSlot)
        - (reinterpret_cast<std::intptr_t>(bytes.data() + callOffset) + 6);
    if (displacement < std::numeric_limits<std::int32_t>::min()
        || displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(bytes.data() + callOffset + 2, &relative, sizeof(relative));
    return true;
}

bool BuildCallsites(FixtureCallsite* const targets) noexcept {
    auto** const importSlot = FindCreateFileImportSlot();
    const std::array<std::uint8_t, kFingerprintSize> archiveSuffix{
        0xff, 0x15, 0, 0, 0, 0, 0x48, 0x89, 0x46, 0x60, 0x48, 0x83, 0xf8, 0xff,
    };
    const std::array<std::uint8_t, kFingerprintSize> saveSuffix{
        0x33, 0xd2, 0xff, 0x15, 0, 0, 0, 0, 0x48, 0x8b, 0xf8, 0x40, 0xb6, 0x01,
    };
    if (!PopulateCallsite(callsiteBytes[0], 0, importSlot, archiveSuffix)
        || !PopulateCallsite(callsiteBytes[1], 2, importSlot, saveSuffix)) {
        return false;
    }
    for (std::size_t index = 0; index < targets->expected.size(); ++index) {
        targets[0].expected[index] = static_cast<std::uint8_t>(callsiteBytes[0][index]);
        targets[1].expected[index] = static_cast<std::uint8_t>(callsiteBytes[1][index]);
    }
    targets[0].address = callsiteBytes[0].data();
    targets[0].pathArgumentIndex = 0;
    targets[1].address = callsiteBytes[1].data();
    targets[1].pathArgumentIndex = 2;
    return true;
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

    const auto logicalSave = Join(
        Join(Join(Join(realDocuments, L"NBGI"), L"DARK SOULS REMASTERED"),
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
    const auto redirected = CreateFileW(
        normalSave.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (redirected == INVALID_HANDLE_VALUE) {
        return 11;
    }
    CloseHandle(redirected);
    return 0;
}

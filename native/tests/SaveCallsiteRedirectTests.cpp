#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "save/SaveCallsiteRedirect.h"

namespace fs = std::filesystem;
using DSRRandomizer::Save::SaveCallsiteRedirectConfiguration;
using DSRRandomizer::Save::SaveCallsiteRedirectInstallStatus;
using DSRRandomizer::Save::SaveCallsiteRedirectTarget;

namespace {

constexpr std::array<std::uint8_t, 14> kArchiveCallsite{
    0xff, 0x15, 0x57, 0x22, 0x31, 0x01,
    0x48, 0x89, 0x46, 0x60, 0x48, 0x83, 0xf8, 0xff,
};
constexpr std::array<std::uint8_t, 14> kSaveCallsite{
    0x33, 0xd2, 0xff, 0x15, 0x75, 0x2e, 0x31, 0x01,
    0x48, 0x8b, 0xf8, 0x40, 0xb6, 0x01,
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto base = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = base / (L"DSRForMod-SaveCallsite-"
                + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(GetTickCount64()) + L"-"
                + std::to_wstring(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) {
                return;
            }
        }
        path_.clear();
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& Path() const noexcept { return path_; }

private:
    fs::path path_;
};

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

bool WriteFile(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

std::string ReadHandle(const HANDLE file) {
    std::array<char, 64> buffer{};
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        return {};
    }
    return std::string(buffer.data(), read);
}

int VerifyExactGameCallsitesRedirectOnlyTheNormalSave() {
    TemporaryDirectory root;
    if (root.Path().empty()) {
        return Fail("temporary directory creation failed");
    }
    const auto normalSave = root.Path() / L"DRAKS0005.sl2";
    const auto dedicatedSave = root.Path() / L"DRAKS0005.rmm";
    const auto unrelated = root.Path() / L"GraphicsConfig.xml";
    if (!WriteFile(normalSave, "normal-save")
        || !WriteFile(dedicatedSave, "dedicated-save")
        || !WriteFile(unrelated, "graphics-config")) {
        return Fail("save redirect fixture creation failed");
    }

    auto* image = static_cast<std::byte*>(VirtualAlloc(
        nullptr,
        0x4000,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (image == nullptr) {
        return Fail("synthetic image allocation failed");
    }
    auto* archive = image + 0x1000;
    auto* save = image + 0x2000;
    auto archiveExpected = kArchiveCallsite;
    auto saveExpected = kSaveCallsite;
    auto** importSlot = reinterpret_cast<void**>(image + 0x3000);
    *importSlot = reinterpret_cast<void*>(&CreateFileW);
    const auto pointCallAtSlot = [importSlot](
        std::array<std::uint8_t, 14>& bytes,
        std::byte* target,
        const std::size_t callOffset) {
        const auto displacement = static_cast<std::int32_t>(
            reinterpret_cast<std::intptr_t>(importSlot)
            - reinterpret_cast<std::intptr_t>(target + callOffset + 6));
        std::memcpy(bytes.data() + callOffset + 2, &displacement, sizeof(displacement));
    };
    pointCallAtSlot(archiveExpected, archive, 0);
    pointCallAtSlot(saveExpected, save, 2);
    std::memcpy(archive, archiveExpected.data(), archiveExpected.size());
    std::memcpy(save, saveExpected.data(), saveExpected.size());

    const SaveCallsiteRedirectConfiguration configuration{
        dedicatedSave.native(),
        {
            SaveCallsiteRedirectTarget{archive, archiveExpected, 0},
            SaveCallsiteRedirectTarget{save, saveExpected, 2},
        },
    };
    const auto installStatus =
        DSRRandomizer::Save::InstallSaveCallsiteRedirect(configuration);
    if (installStatus != SaveCallsiteRedirectInstallStatus::Success) {
        std::cerr << "install status="
                  << static_cast<int>(installStatus) << '\n';
        VirtualFree(image, 0, MEM_RELEASE);
        return Fail("exact callsite redirect installation failed");
    }
    if (std::memcmp(archive, archiveExpected.data(), archiveExpected.size()) != 0
        || std::memcmp(save, saveExpected.data(), saveExpected.size()) != 0
        || *importSlot == reinterpret_cast<void*>(&CreateFileW)) {
        static_cast<void>(
            DSRRandomizer::Save::UninstallSaveCallsiteRedirect());
        VirtualFree(image, 0, MEM_RELEASE);
        return Fail("the import slot was not redirected without changing game code");
    }

    using CreateFileFunction = HANDLE(WINAPI*)(
        LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    const auto redirectedCreateFile = reinterpret_cast<CreateFileFunction>(*importSlot);
    const HANDLE redirected = redirectedCreateFile(
        normalSave.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const auto redirectedContents = redirected == INVALID_HANDLE_VALUE
        ? std::string{}
        : ReadHandle(redirected);
    if (redirected != INVALID_HANDLE_VALUE) {
        CloseHandle(redirected);
    }
    const HANDLE passthrough = redirectedCreateFile(
        unrelated.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const auto passthroughContents = passthrough == INVALID_HANDLE_VALUE
        ? std::string{}
        : ReadHandle(passthrough);
    if (passthrough != INVALID_HANDLE_VALUE) {
        CloseHandle(passthrough);
    }

    const auto cleanup = DSRRandomizer::Save::UninstallSaveCallsiteRedirect();
    const bool restored = std::memcmp(
            archive, archiveExpected.data(), archiveExpected.size()) == 0
        && std::memcmp(save, saveExpected.data(), saveExpected.size()) == 0
        && *importSlot == reinterpret_cast<void*>(&CreateFileW);
    VirtualFree(image, 0, MEM_RELEASE);

    if (redirectedContents != "dedicated-save") {
        return Fail("DRAKS0005.sl2 was not redirected to the dedicated rmm");
    }
    if (passthroughContents != "graphics-config") {
        return Fail("an unrelated file was redirected");
    }
    if (cleanup
            != DSRRandomizer::Save::SaveCallsiteRedirectCleanupStatus::Success
        || !restored) {
        return Fail("callsite bytes were not restored exactly");
    }
    return 0;
}

}  // namespace

int wmain() {
    return VerifyExactGameCallsitesRedirectOnlyTheNormalSave();
}

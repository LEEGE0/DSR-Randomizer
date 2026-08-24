#include <cstdint>
#include <iostream>
#include <new>
#include <string>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "ProtectionBootstrap.h"

namespace {

static_assert(sizeof(DSRRandomizer::ProtectionInitBlock) == 5428);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, pipeName) == 52);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, virtualDocuments) == 308);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, virtualLogicalSave) == 1332);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, realSaveRoot) == 2356);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, externalSaveRoot) == 3380);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, dedicatedRmm) == 4404);

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

bool ThrowingPathReader(
    const wchar_t*,
    std::size_t,
    std::wstring&) {
    throw std::bad_alloc();
}

}  // namespace

int main() {
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = 99;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap);

    const auto status = DSRRandomizer::InitializeForTest(&block);
    if (status != DSRRandomizer::InitStatus::UnsupportedProtocol) {
        return Fail("wrong protocol was not rejected");
    }

    if (DSRRandomizer::CurrentProtectionFlags()
        != DSRRandomizer::ProtectionFlags::None) {
        return Fail("failed initialization left protection flags active");
    }

    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block) - sizeof(wchar_t));
    const auto wrongSizeStatus = DSRRandomizer::InitializeForTest(&block);
    if (wrongSizeStatus != DSRRandomizer::InitStatus::InvalidArgument) {
        return Fail("wrong protocol block size was not rejected");
    }

    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::SaveFileIo);
    const auto partialSaveFlagsStatus = DSRRandomizer::InitializeForTest(&block);
    if (partialSaveFlagsStatus
        != DSRRandomizer::InitStatus::RequiredProtectionUnavailable) {
        return Fail("partial save-hook group was not rejected");
    }

    block.requiredFlags |= static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::SaveKnownFolder);
    const auto missingSaveConfigurationStatus =
        DSRRandomizer::InitializeForTest(&block);
    if (missingSaveConfigurationStatus
        != DSRRandomizer::InitStatus::SaveHookInstallFailed) {
        return Fail("missing save-hook configuration did not report SAVE_HOOK_INSTALL_FAILED");
    }
    if (DSRRandomizer::CurrentProtectionFlags()
        != DSRRandomizer::ProtectionFlags::None) {
        return Fail("failed save-hook installation left protection flags active");
    }

    block.virtualDocuments[0] = L'x';
    block.virtualDocuments[1] = L'\0';
    const auto throwingSaveConfigurationStatus =
        DSRRandomizer::Testing::InitializeWithPathReader(
            &block,
            &ThrowingPathReader);
    if (throwingSaveConfigurationStatus
        != DSRRandomizer::InitStatus::SaveHookInstallFailed) {
        return Fail("save configuration allocation failure escaped InitializeCore");
    }
    if (DSRRandomizer::CurrentProtectionFlags()
        != DSRRandomizer::ProtectionFlags::None) {
        return Fail("save configuration exception left protection flags active");
    }

    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap);
    const auto successStatus = DSRRandomizer::InitializeForTest(&block);
    if (successStatus != DSRRandomizer::InitStatus::Success) {
        return Fail("supported protocol did not initialize");
    }

    if (DSRRandomizer::CurrentProtectionFlags()
        != DSRRandomizer::ProtectionFlags::Bootstrap) {
        return Fail("successful initialization did not activate bootstrap protection");
    }

    return 0;
}

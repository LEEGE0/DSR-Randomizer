#include <winsock2.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <new>
#include <string>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "ProtectionBootstrap.h"
#include "modules/DeferredModuleGate.h"
#include "network/WinsockHooks.h"

namespace {

static_assert(sizeof(DSRRandomizer::ProtectionSocketEndpoint) == 24);
static_assert(sizeof(DSRRandomizer::ProtectionInitBlock) == 5480);
static_assert(DSRRandomizer::kSimplifiedOfflineRequiredFlags == 0x7FULL);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, pipeName) == 52);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, virtualDocuments) == 308);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, virtualLogicalSave) == 1332);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, realSaveRoot) == 2356);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, externalSaveRoot) == 3380);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, dedicatedRmm) == 4404);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, socketEndpointCount) == 5428);
static_assert(offsetof(DSRRandomizer::ProtectionInitBlock, socketEndpoints) == 5432);

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

class HarmlessPipeFixture final {
public:
    HarmlessPipeFixture() {
        pipeName_ = L"\\\\.\\pipe\\DSRRandomizer-ProtectionBootstrapTests-"
            + std::to_wstring(GetCurrentProcessId());
        pipe_ = CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            0,
            sizeof(DSRRandomizer::ProtectionHandshakeMessage),
            0,
            nullptr);
    }

    ~HarmlessPipeFixture() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
        }
    }

    HarmlessPipeFixture(const HarmlessPipeFixture&) = delete;
    HarmlessPipeFixture& operator=(const HarmlessPipeFixture&) = delete;

    [[nodiscard]] bool IsValid() const noexcept {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] const std::wstring& Name() const noexcept {
        return pipeName_;
    }

private:
    std::wstring pipeName_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

DSRRandomizer::ProtectionInitBlock ProductionBlock(
    const std::wstring& pipeName,
    const std::uint64_t requiredFlags) {
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = requiredFlags;
    wcsncpy_s(block.pipeName, pipeName.c_str(), _TRUNCATE);
    constexpr wchar_t path[] = L"C:\\fixture";
    wcsncpy_s(block.virtualDocuments, path, _TRUNCATE);
    wcsncpy_s(block.virtualLogicalSave, path, _TRUNCATE);
    wcsncpy_s(block.realSaveRoot, path, _TRUNCATE);
    wcsncpy_s(block.externalSaveRoot, path, _TRUNCATE);
    wcsncpy_s(block.dedicatedRmm, path, _TRUNCATE);
    return block;
}

int VerifySimplifiedProductionBitmap() {
    HarmlessPipeFixture pipe;
    if (!pipe.IsValid()) {
        return Fail("harmless supervisor pipe fixture could not be created");
    }

    constexpr auto simplified = 0x7FULL;
    struct RejectedCase {
        const char* name;
        std::uint64_t flags;
    };
    std::array<RejectedCase, 10> rejected{};
    for (std::size_t bit = 0; bit < 7; ++bit) {
        rejected[bit] = {
            "missing simplified protection bit",
            simplified & ~(1ULL << bit),
        };
    }
    rejected[7] = {"unexpected heartbeat bit", simplified | (1ULL << 7)};
    rejected[8] = {"unexpected hook-integrity bit", simplified | (1ULL << 8)};
    rejected[9] = {"unexpected unknown high bit", simplified | (1ULL << 40)};

    for (const auto& test : rejected) {
        auto candidate = ProductionBlock(pipe.Name(), test.flags);
        const auto actual = DSRRandomizer::InitializeProtection(&candidate);
        if (actual != DSRRandomizer::InitStatus::RequiredProtectionUnavailable
            || DSRRandomizer::CurrentProtectionFlags()
                != DSRRandomizer::ProtectionFlags::None) {
            std::cerr << "production bitmap rejection failed: " << test.name
                      << ", flags=" << test.flags
                      << ", actual=" << static_cast<unsigned int>(actual) << '\n';
            return 1;
        }
    }

    auto exact = ProductionBlock(pipe.Name(), simplified);
    const auto exactStatus = DSRRandomizer::InitializeProtection(&exact);
    if (exactStatus != DSRRandomizer::InitStatus::GameServiceProfileMismatch) {
        return Fail("exact simplified bitmap did not enter production core initialization");
    }
    return 0;
}

bool ThrowingPathReader(
    const wchar_t*,
    std::size_t,
    std::wstring&) {
    throw std::bad_alloc();
}

bool MissingSteamConfiguration(
    DSRRandomizer::Modules::DeferredModuleGateConfiguration&) noexcept {
    return false;
}

DSRRandomizer::ProtectionInitBlock ValidWinsockBlock() {
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = DSRRandomizer::kProtectionProtocolVersion;
    block.size = static_cast<std::uint16_t>(sizeof(block));
    block.requiredFlags = static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(DSRRandomizer::ProtectionFlags::Winsock);
    block.socketEndpointCount = 1;
    auto& endpoint = block.socketEndpoints[0];
    endpoint.transport = static_cast<std::uint16_t>(
        DSRRandomizer::SocketTransport::Tcp);
    endpoint.family = AF_INET;
    endpoint.port = htons(42000);
    endpoint.address[0] = 127;
    endpoint.address[1] = 0;
    endpoint.address[2] = 0;
    endpoint.address[3] = 1;
    return block;
}

void StaleSameVersionSize(DSRRandomizer::ProtectionInitBlock& block) {
    --block.size;
}
void EndpointCountOverflow(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpointCount =
        static_cast<std::uint32_t>(DSRRandomizer::kProtectionSocketEndpointCapacity + 1);
}
void ReservedBytes(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].reserved = 1;
}
void NonzeroUnusedSlot(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[1].address[0] = 1;
}
void UnknownTransport(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].transport = 99;
}
void UnknownFamily(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].family = 0x7fff;
}
void DuplicateTransport(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpointCount = 2;
    block.socketEndpoints[1] = block.socketEndpoints[0];
    block.socketEndpoints[1].port = htons(42001);
}
void ZeroPort(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].port = 0;
}
void NonLoopbackAddress(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].address[0] = 8;
    block.socketEndpoints[0].address[1] = 8;
    block.socketEndpoints[0].address[2] = 8;
    block.socketEndpoints[0].address[3] = 8;
}
void IPv4Padding(DSRRandomizer::ProtectionInitBlock& block) {
    block.socketEndpoints[0].address[4] = 1;
}

int VerifyInvalidWinsockBlocks() {
    using Mutator = void(*)(DSRRandomizer::ProtectionInitBlock&);
    struct InvalidCase {
        const char* name;
        Mutator mutate;
        DSRRandomizer::InitStatus expected;
    };
    const std::array cases{
        InvalidCase{"stale same-version size", &StaleSameVersionSize,
            DSRRandomizer::InitStatus::InvalidArgument},
        InvalidCase{"endpoint count overflow", &EndpointCountOverflow,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"reserved bytes", &ReservedBytes,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"nonzero unused slot", &NonzeroUnusedSlot,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"unknown transport", &UnknownTransport,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"unknown family", &UnknownFamily,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"duplicate transport", &DuplicateTransport,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"zero port", &ZeroPort,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"non-loopback address", &NonLoopbackAddress,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
        InvalidCase{"IPv4 padding", &IPv4Padding,
            DSRRandomizer::InitStatus::WinsockHookInstallFailed},
    };

    for (const auto& test : cases) {
        auto candidate = ValidWinsockBlock();
        test.mutate(candidate);
        const auto actual = DSRRandomizer::InitializeForTest(&candidate);
        if (actual != test.expected
            || DSRRandomizer::CurrentProtectionFlags()
                != DSRRandomizer::ProtectionFlags::None
            || DSRRandomizer::Network::WinsockHooksAreInstalled()) {
            std::cerr << "invalid Winsock block case failed: " << test.name
                      << ", expected=" << static_cast<unsigned int>(test.expected)
                      << ", actual=" << static_cast<unsigned int>(actual) << '\n';
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main() {
    if (const auto simplifiedResult = VerifySimplifiedProductionBitmap();
        simplifiedResult != 0) {
        return simplifiedResult;
    }

    if (const auto invalidWinsockResult = VerifyInvalidWinsockBlocks();
        invalidWinsockResult != 0) {
        return invalidWinsockResult;
    }

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
        DSRRandomizer::ProtectionFlags::Bootstrap)
        | static_cast<std::uint64_t>(
            DSRRandomizer::ProtectionFlags::SteamInterfaces);
    const auto partialSteamFlagsStatus = DSRRandomizer::InitializeForTest(&block);
    if (partialSteamFlagsStatus
        != DSRRandomizer::InitStatus::RequiredProtectionUnavailable) {
        return Fail("partial Steam/deferred protection group was not rejected");
    }

    block.requiredFlags |= static_cast<std::uint64_t>(
        DSRRandomizer::ProtectionFlags::DeferredModuleGate);
    const auto productionSteamStatus = DSRRandomizer::InitializeForTest(&block);
    if (productionSteamStatus
            != DSRRandomizer::InitStatus::SteamConfigurationUnavailable
        || DSRRandomizer::Modules::DeferredModuleGateIsInstalled()) {
        return Fail("Steam flags without an authoritative provider did not fail closed");
    }

    const auto missingProviderStatus =
        DSRRandomizer::Testing::InitializeWithSteamConfigurationProvider(
            &block,
            &MissingSteamConfiguration);
    if (missingProviderStatus
            != DSRRandomizer::InitStatus::SteamConfigurationUnavailable
        || DSRRandomizer::CurrentProtectionFlags()
            != DSRRandomizer::ProtectionFlags::None
        || DSRRandomizer::Modules::DeferredModuleGateIsInstalled()) {
        return Fail("rejected synthetic Steam provider left partial protection");
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

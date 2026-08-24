#include <cstdint>
#include <iostream>

#include "DSRRandomizer/ProtectionProtocol.h"
#include "ProtectionBootstrap.h"

namespace {

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main() {
    DSRRandomizer::ProtectionInitBlock block{};
    block.magic = DSRRandomizer::kProtectionMagic;
    block.version = 99;
    block.size = static_cast<std::uint16_t>(sizeof(block));

    const auto status = DSRRandomizer::InitializeForTest(&block);
    if (status != DSRRandomizer::InitStatus::UnsupportedProtocol) {
        return Fail("wrong protocol was not rejected");
    }

    if (DSRRandomizer::CurrentProtectionFlags()
        != DSRRandomizer::ProtectionFlags::None) {
        return Fail("failed initialization left protection flags active");
    }

    block.version = DSRRandomizer::kProtectionProtocolVersion;
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

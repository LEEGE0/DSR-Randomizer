#include "ProtectionBootstrap.h"

#include <atomic>

namespace DSRRandomizer {
namespace {

std::atomic<std::uint64_t> activeFlags{0};

}  // namespace

InitStatus InitializeProtection(ProtectionInitBlock* block) noexcept {
    activeFlags.store(0, std::memory_order_release);
    if (block == nullptr || block->size != sizeof(ProtectionInitBlock)) {
        return InitStatus::InvalidArgument;
    }

    if (block->magic != kProtectionMagic
        || block->version != kProtectionProtocolVersion) {
        return InitStatus::UnsupportedProtocol;
    }

    activeFlags.store(
        static_cast<std::uint64_t>(ProtectionFlags::Bootstrap),
        std::memory_order_release);
    return InitStatus::Success;
}

InitStatus InitializeForTest(ProtectionInitBlock* block) noexcept {
    return InitializeProtection(block);
}

ProtectionFlags CurrentProtectionFlags() noexcept {
    return static_cast<ProtectionFlags>(activeFlags.load(std::memory_order_acquire));
}

}  // namespace DSRRandomizer

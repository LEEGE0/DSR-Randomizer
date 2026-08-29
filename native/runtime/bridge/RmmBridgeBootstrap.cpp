#include "bridge/RmmBridgeBootstrap.h"

#include <exception>

namespace DSRRandomizer::Bridge {
namespace {

BridgeBootstrapResult Failure(std::uint32_t exitCode, std::wstring message) {
    return BridgeBootstrapResult{false, exitCode, std::move(message)};
}

}  // namespace

BridgeBootstrapResult BootstrapRmmBridge(BridgeBootstrapPlatform& platform) noexcept {
    try {
        const auto resolved = platform.ResolveConfiguration();
        if (!resolved.ok) {
            platform.WriteFailureLog(nullptr, resolved.message);
            return Failure(101, resolved.message);
        }

        std::wstring message;
        if (!platform.StartHostAndWaitReady(resolved.value, message)) {
            platform.WriteFailureLog(&resolved.value, message);
            return Failure(102, std::move(message));
        }

        const Save::SaveHookConfiguration hooks{
            resolved.value.virtualDocuments,
            resolved.value.virtualLogicalSave,
            resolved.value.realSaveRoot,
            resolved.value.externalSaveRoot,
            resolved.value.dedicatedRmm,
            true,
            false,
        };
        if (!platform.InstallHooks(hooks, message)) {
            platform.WriteFailureLog(&resolved.value, message);
            return Failure(103, std::move(message));
        }
        return BridgeBootstrapResult{true, 0, L""};
    } catch (const std::exception&) {
        platform.WriteFailureLog(nullptr, L"Unhandled native bridge bootstrap failure.");
        return Failure(104, L"Unhandled native bridge bootstrap failure.");
    } catch (...) {
        platform.WriteFailureLog(nullptr, L"Unknown native bridge bootstrap failure.");
        return Failure(105, L"Unknown native bridge bootstrap failure.");
    }
}

}  // namespace DSRRandomizer::Bridge

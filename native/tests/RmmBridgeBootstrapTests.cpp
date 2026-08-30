#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "bridge/RmmBridgeBootstrap.h"
#include "bridge/RmmBridgeHostClient.h"
#include "bridge/WindowsBridgePlatform.h"

namespace {

using DSRRandomizer::Bridge::BridgeBootstrapPlatform;
using DSRRandomizer::Bridge::BridgeConfiguration;
using DSRRandomizer::Bridge::BridgeConfigurationError;
using DSRRandomizer::Bridge::BridgeConfigurationResult;
using DSRRandomizer::Bridge::BootstrapRmmBridge;
using DSRRandomizer::Bridge::BuildHostCommandLine;
using DSRRandomizer::Bridge::DeriveExternalRootFromBridgeModulePath;
using DSRRandomizer::Save::SaveHookConfiguration;

class FakePlatform final : public BridgeBootstrapPlatform {
public:
    BridgeConfigurationResult ResolveConfiguration() override {
        calls.emplace_back("resolve");
        if (!resolveSucceeds) {
            return {false, {}, BridgeConfigurationError::LayoutInvalid, L"bad layout"};
        }
        return {true,
                BridgeConfiguration{
                    LR"(D:\root\runtimes\runtime-id)", LR"(D:\root)", L"runtime-id",
                    L"146808034", LR"(D:\root\profile)",
                    LR"(D:\root\profile\NBGI\DARK SOULS REMASTERED\146808034\DRAKS0005.sl2)",
                    LR"(C:\Users\User\Documents\NBGI\DARK SOULS REMASTERED)",
                    LR"(D:\root\saves\146808034)",
                    LR"(D:\root\saves\146808034\DRAKS0005.rmm)",
                    LR"(D:\root\components\rmm-bridge\DSRRandomizer.RmmBridgeHost.exe)",
                    "save-id", "metadata-id",
                    LR"(C:\Steam\DARK SOULS REMASTERED\overhaul\GameParam.parambnd.dcx)",
                    LR"(D:\root\components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx)"},
                BridgeConfigurationError::None,
                L""};
    }

    bool StartHostAndWaitReady(const BridgeConfiguration&, std::wstring& message) override {
        calls.emplace_back("host-ready");
        message = hostSucceeds ? L"" : L"host failed";
        return hostSucceeds;
    }

    bool InstallHooks(const SaveHookConfiguration& configuration,
                      std::wstring& message) override {
        calls.emplace_back("install-hooks");
        installedConfiguration = configuration;
        message = hooksSucceed ? L"" : L"hooks failed";
        return hooksSucceed;
    }

    void WriteFailureLog(const BridgeConfiguration*, std::wstring_view) override {
        calls.emplace_back("log");
    }

    bool resolveSucceeds{true};
    bool hostSucceeds{true};
    bool hooksSucceed{true};
    std::vector<std::string> calls;
    SaveHookConfiguration installedConfiguration{};
};

int Fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main() {
    if (DeriveExternalRootFromBridgeModulePath(
            LR"(D:\root\components\rmm-bridge\DSRRandomizer.RmmBridge.dll)")
            != LR"(D:\root)"
        || !DeriveExternalRootFromBridgeModulePath(
                LR"(D:\root\rmm-bridge\DSRRandomizer.RmmBridge.dll)").empty()) {
        return Fail("bridge module path did not derive the bounded external root");
    }

    const BridgeConfiguration commandConfiguration{
        LR"(D:\root with spaces\runtimes\runtime-id)", LR"(D:\root with spaces)",
        L"runtime-id", L"146808034", L"", L"", L"", L"", L"",
        LR"(D:\root with spaces\components\rmm-bridge\DSRRandomizer.RmmBridgeHost.exe)",
        "", ""};
    const auto commandLine = BuildHostCommandLine(
        commandConfiguration,
        4242,
        LR"(Local\DSRRandomizer.RmmBridge.0123456789abcdef0123456789abcdef)");
    if (commandLine.find(
            LR"("D:\root with spaces\components\rmm-bridge\DSRRandomizer.RmmBridgeHost.exe")")
            != 0
        || commandLine.find(LR"(--external-root "D:\root with spaces")")
            == std::wstring::npos) {
        return Fail("host command line did not quote paths containing spaces");
    }

    FakePlatform success;
    const auto result = BootstrapRmmBridge(success);
    if (!result.ok
        || success.calls != std::vector<std::string>{"resolve", "host-ready", "install-hooks"}) {
        return Fail("bootstrap did not preserve resolve-host-hook order");
    }
    if (!success.installedConfiguration.protectFileIo
        || success.installedConfiguration.diagnosticMode
        || success.installedConfiguration.dedicatedRmm
            != LR"(D:\root\saves\146808034\DRAKS0005.rmm)"
        || success.installedConfiguration.overhaulGameParamSource
            != LR"(C:\Steam\DARK SOULS REMASTERED\overhaul\GameParam.parambnd.dcx)"
        || success.installedConfiguration.overhaulGameParamTarget
            != LR"(D:\root\components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx)") {
        return Fail("bootstrap installed the wrong save-hook configuration");
    }

    const SaveHookConfiguration normalGuardedLaunch{
        L"virtual-documents", L"virtual-save", L"real-save-root",
        L"external-save-root", L"dedicated.rmm", true, false};
    if (!normalGuardedLaunch.overhaulGameParamSource.empty()
        || !normalGuardedLaunch.overhaulGameParamTarget.empty()) {
        return Fail("normal guarded launch did not keep GameParam redirect disabled");
    }

    FakePlatform hostFailure;
    hostFailure.hostSucceeds = false;
    if (BootstrapRmmBridge(hostFailure).ok
        || hostFailure.calls
            != std::vector<std::string>{"resolve", "host-ready", "log"}) {
        return Fail("host failure did not stop before hook installation and log once");
    }

    FakePlatform hookFailure;
    hookFailure.hooksSucceed = false;
    if (BootstrapRmmBridge(hookFailure).ok
        || hookFailure.calls
            != std::vector<std::string>{"resolve", "host-ready", "install-hooks", "log"}) {
        return Fail("hook failure was not logged");
    }

    FakePlatform resolveFailure;
    resolveFailure.resolveSucceeds = false;
    if (BootstrapRmmBridge(resolveFailure).ok
        || resolveFailure.calls != std::vector<std::string>{"resolve", "log"}) {
        return Fail("configuration failure was not logged before other actions");
    }

    return 0;
}

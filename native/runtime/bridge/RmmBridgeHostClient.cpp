#include "bridge/RmmBridgeHostClient.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

#include "DSRRandomizer/RmmBridgeProtocol.h"

namespace DSRRandomizer::Bridge {
namespace {

HANDLE retainedHostProcess = nullptr;

std::wstring Quote(std::wstring_view value) {
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring CreateReadyEventName() {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(
            nullptr,
            random.data(),
            static_cast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return {};
    }
    std::wostringstream stream;
    stream << Protocol::ReadyEventPrefix << std::hex << std::setfill(L'0');
    for (const auto value : random) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

}  // namespace

std::wstring BuildHostCommandLine(
    const BridgeConfiguration& configuration,
    const std::uint32_t gamePid,
    const std::wstring& readyEventName) {
    return Quote(configuration.hostExecutable)
        + L" --game-pid " + std::to_wstring(gamePid)
        + L" --external-root " + Quote(configuration.externalRoot)
        + L" --runtime-id " + Quote(configuration.runtimeId)
        + L" --steam-id " + Quote(configuration.steamId)
        + L" --ready-event " + Quote(readyEventName);
}

bool StartRmmBridgeHostAndWaitReady(
    const BridgeConfiguration& configuration,
    std::wstring& message) {
    const auto eventName = CreateReadyEventName();
    if (eventName.empty()) {
        message = L"Unable to generate the bridge ready-event name.";
        return false;
    }

    const auto readyEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (readyEvent == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (readyEvent != nullptr) {
            CloseHandle(readyEvent);
        }
        message = L"Unable to create a unique bridge ready event.";
        return false;
    }

    auto commandLine = BuildHostCommandLine(
        configuration, GetCurrentProcessId(), eventName);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(
        configuration.hostExecutable.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        configuration.externalRoot.c_str(),
        &startup,
        &process);
    if (!created) {
        CloseHandle(readyEvent);
        message = L"Unable to start DSRRandomizer.RmmBridgeHost.exe.";
        return false;
    }
    CloseHandle(process.hThread);

    const HANDLE waitHandles[]{readyEvent, process.hProcess};
    const auto wait = WaitForMultipleObjects(
        2, waitHandles, FALSE, Protocol::ReadyTimeoutMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 106);
        WaitForSingleObject(process.hProcess, 2'000);
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        message = wait == WAIT_OBJECT_0 + 1
            ? L"The bridge host exited before reporting readiness."
            : L"The bridge host readiness handshake timed out.";
        return false;
    }

    CloseHandle(readyEvent);
    retainedHostProcess = process.hProcess;
    return true;
}

}  // namespace DSRRandomizer::Bridge

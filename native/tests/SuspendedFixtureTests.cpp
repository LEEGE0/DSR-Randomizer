#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

int Fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

std::wstring Quote(std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        return Fail("fixture executable path argument is missing");
    }

    const auto eventName = L"Local\\DSRRandomizerFixtureTest-"
        + std::to_wstring(GetCurrentProcessId());
    const HANDLE eventHandle = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (eventHandle == nullptr) {
        return Fail("unable to create fixture event");
    }

    auto commandLine = Quote(argv[1]) + L" " + Quote(eventName);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        argv[1],
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created) {
        CloseHandle(eventHandle);
        return Fail("unable to start fixture process");
    }

    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 200) != WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(eventHandle);
        return Fail("fixture exited before its event was signaled");
    }

    SetEvent(eventHandle);
    const auto waitResult = WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(eventHandle);

    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        return Fail("fixture did not exit cleanly after its event was signaled");
    }

    return 0;
}

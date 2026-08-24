#include <Windows.h>

#include <iterator>
#include <string>
#include <vector>

namespace {

int SpawnChildAndWait() {
    wchar_t executablePath[MAX_PATH]{};
    const auto pathLength = GetModuleFileNameW(
        nullptr,
        executablePath,
        static_cast<DWORD>(std::size(executablePath)));
    if (pathLength == 0 || pathLength >= std::size(executablePath)) {
        return 5;
    }

    const std::wstring commandLine = L"\"" + std::wstring(executablePath)
        + L"\" \"--child\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(
            executablePath,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup,
            &child)) {
        return 6;
    }

    wchar_t temporaryPath[MAX_PATH]{};
    const auto temporaryPathLength = GetTempPathW(
        static_cast<DWORD>(std::size(temporaryPath)),
        temporaryPath);
    if (temporaryPathLength == 0 || temporaryPathLength >= std::size(temporaryPath)) {
        TerminateProcess(child.hProcess, 7);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 7;
    }

    const auto pidPath = std::wstring(temporaryPath)
        + L"DSRRandomizerFixtureChild-"
        + std::to_wstring(GetCurrentProcessId())
        + L".txt";
    const HANDLE pidFile = CreateFileW(
        pidPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pidFile == INVALID_HANDLE_VALUE) {
        TerminateProcess(child.hProcess, 8);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 8;
    }

    const auto pidText = std::to_string(child.dwProcessId);
    DWORD bytesWritten = 0;
    const BOOL wrote = WriteFile(
        pidFile,
        pidText.data(),
        static_cast<DWORD>(pidText.size()),
        &bytesWritten,
        nullptr);
    const BOOL flushed = wrote ? FlushFileBuffers(pidFile) : FALSE;
    CloseHandle(pidFile);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (!wrote || !flushed || bytesWritten != pidText.size()) {
        return 9;
    }

    Sleep(INFINITE);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 1) {
        wchar_t value[2]{};
        const auto length = GetEnvironmentVariableW(
            L"DSR_RANDOMIZER_PARENT_SENTINEL",
            value,
            static_cast<DWORD>(std::size(value)));
        return length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : 42;
    }

    if (argc != 2) {
        return 2;
    }

    if (std::wstring_view(argv[1]) == L"--child") {
        Sleep(INFINITE);
        return 0;
    }

    if (std::wstring_view(argv[1]) == L"--spawn-child") {
        return SpawnChildAndWait();
    }

    const HANDLE eventHandle = OpenEventW(SYNCHRONIZE, FALSE, argv[1]);
    if (eventHandle == nullptr) {
        return 3;
    }

    const auto waitResult = WaitForSingleObject(eventHandle, INFINITE);
    CloseHandle(eventHandle);
    return waitResult == WAIT_OBJECT_0 ? 0 : 4;
}

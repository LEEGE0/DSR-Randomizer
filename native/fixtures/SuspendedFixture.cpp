#include <Windows.h>

#include <iterator>

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

    const HANDLE eventHandle = OpenEventW(SYNCHRONIZE, FALSE, argv[1]);
    if (eventHandle == nullptr) {
        return 3;
    }

    const auto waitResult = WaitForSingleObject(eventHandle, INFINITE);
    CloseHandle(eventHandle);
    return waitResult == WAIT_OBJECT_0 ? 0 : 4;
}

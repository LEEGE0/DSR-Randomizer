#include <Windows.h>

int wmain(int argc, wchar_t* argv[]) {
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

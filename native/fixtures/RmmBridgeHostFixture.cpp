#include <Windows.h>

#include <string>

int wmain(int argc, wchar_t** argv) {
    std::wstring readyEvent;
    DWORD gamePid{};
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::wstring name(argv[index]);
        if (name == L"--ready-event") {
            readyEvent = argv[index + 1];
        } else if (name == L"--game-pid") {
            gamePid = static_cast<DWORD>(_wtoi(argv[index + 1]));
        }
    }
    if (readyEvent.empty() || gamePid == 0) {
        return 2;
    }
    const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEvent.c_str());
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    if (event == nullptr || process == nullptr) {
        if (event != nullptr) {
            CloseHandle(event);
        }
        if (process != nullptr) {
            CloseHandle(process);
        }
        return 3;
    }
    const auto signaled = SetEvent(event);
    CloseHandle(event);
    if (!signaled) {
        CloseHandle(process);
        return 4;
    }
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    return 0;
}

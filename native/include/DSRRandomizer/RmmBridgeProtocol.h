#pragma once

#include <Windows.h>

namespace DSRRandomizer::Bridge::Protocol {

inline constexpr DWORD ReadyTimeoutMilliseconds = 15'000;
inline constexpr wchar_t HostFileName[] = L"DSRRandomizer.RmmBridgeHost.exe";
inline constexpr wchar_t ReadyEventPrefix[] = L"Local\\DSRRandomizer.RmmBridge.";

}  // namespace DSRRandomizer::Bridge::Protocol

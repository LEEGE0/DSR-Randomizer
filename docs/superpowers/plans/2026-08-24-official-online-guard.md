# Official Online Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add fail-closed FromSoftware, Steam, Winsock, deferred-module, heartbeat, and hook-integrity protections to the fixture-tested guard.

**Architecture:** Pure policy modules decide denial independently of MinHook adapters. Winsock containment and main-image game-service blocking are installed before resume; a loader gate prevents deferred Steam modules from exposing protected symbols until verified hooks are active. The guard reports a precise bitmap and one heartbeat per second through the authenticated pipe.

**Tech Stack:** C++20, Windows Sockets 2, Steamworks-compatible synthetic fixture interfaces, MinHook, CTest, System.IO.Pipes, .NET 8

**Spec:** `docs/superpowers/specs/2026-08-24-native-safety-runtime-design.md`

## Global Constraints

- Do not change Windows Firewall, Steam settings, Steam Cloud settings, normal game settings, or Overhaul settings.
- Steam ownership initialization may remain available; matchmaking, P2P/networking, and Remote Storage are denied.
- Non-loopback TCP/UDP is denied; authenticated supervisor loopback is allowed.
- Game-service, save, socket, and supervisor protections cannot be deferred.
- Unknown Steam interface versions and target-fingerprint mismatches fail closed.
- Heartbeat interval is one second; five consecutive misses are fatal.
- No real game process starts in this plan. Use synthetic DLLs and harmless fixtures only.

## File Structure

- `native/runtime/network/NetworkPolicy.{h,cpp}`: endpoint and operation decisions.
- `native/runtime/network/WinsockHooks.{h,cpp}`: socket-layer adapters.
- `native/runtime/steam/SteamPolicy.{h,cpp}`: allowed/denied interface methods.
- `native/runtime/steam/SteamHooks.{h,cpp}`: versioned interface wrappers.
- `native/runtime/modules/DeferredModuleGate.{h,cpp}`: load/path/hash/version admission.
- `native/runtime/game/GameServiceGuard.{h,cpp}`: main-image target validation and offline enforcement.
- `native/runtime/monitor/ProtectionMonitor.{h,cpp}`: hook integrity and heartbeat.
- `native/fixtures/FakeSteamApi.cpp`: synthetic Steam exports/interfaces.
- `native/fixtures/NetworkClientFixture.cpp`: TCP/UDP/Steam attempts.
- `config/compatibility-profiles.json`: release-pinned supported-build profile.
- `tools/DSRRandomizer.ProfileInspector`: PE/import/fingerprint verifier; never modifies the image.
- `src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs`: heartbeat and fatal-event handling.

---

### Task 1: Pure endpoint and network-operation policy

**Files:**
- Create: `native/runtime/network/NetworkPolicy.h`
- Create: `native/runtime/network/NetworkPolicy.cpp`
- Create: `native/tests/NetworkPolicyTests.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Produces: `NetworkDecision EvaluateSocketOperation(SocketOperation, const sockaddr*, int)`.
- Produces: allow only IPv4 `127.0.0.0/8`, IPv6 `::1`, and the exact supervisor endpoint/nonce context.

- [ ] **Step 1: Write the complete endpoint matrix**

```cpp
REQUIRE(Evaluate(Connect, IPv4("127.0.0.1", 42000)) == AllowLoopback);
REQUIRE(Evaluate(Connect, IPv4("8.8.8.8", 53)) == DenyNonLoopback);
REQUIRE(Evaluate(SendTo, IPv6("::1", 42000)) == AllowLoopback);
REQUIRE(Evaluate(SendTo, IPv6("2001:4860:4860::8888", 53)) == DenyNonLoopback);
```

Add IPv4-mapped IPv6, broadcast, multicast, invalid length/family, `AF_UNSPEC`, and DNS-result cases.

- [ ] **Step 2: Run CTest and observe missing policy**

Run: `ctest --preset windows-x64-debug --output-on-failure -R NetworkPolicyTests`

Expected: compile FAIL because the policy does not exist.

- [ ] **Step 3: Implement fail-closed address parsing**

Return deny for malformed or unknown families. Treat IPv4-mapped IPv6 as IPv4 before testing loopback. Do not resolve hostnames inside the hook.

```cpp
NetworkDecision EvaluateSocketOperation(SocketOperation op, const sockaddr* address, int length) {
    const auto parsed = ParseEndpoint(address, length);
    return parsed && parsed->IsLoopback() ? NetworkDecision::AllowLoopback : NetworkDecision::DenyNonLoopback;
}
```

- [ ] **Step 4: Run all native policy tests**

Run: `ctest --preset windows-x64-debug --output-on-failure`

Expected: all native tests PASS.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/network/NetworkPolicy.* native/tests/NetworkPolicyTests.cpp native/CMakeLists.txt
git commit -m "feat: define fail-closed network policy"
```

### Task 2: Winsock containment adapters

**Files:**
- Create: `native/runtime/network/WinsockHooks.h`
- Create: `native/runtime/network/WinsockHooks.cpp`
- Create: `native/fixtures/NetworkClientFixture.cpp`
- Create: `native/tests/WinsockHookIntegrationTests.cpp`
- Modify: `native/include/DSRRandomizer/ProtectionProtocol.h`
- Modify: `native/runtime/ProtectionBootstrap.cpp`

**Interfaces:**
- Adds: `ProtectionFlags::Winsock`.
- Hooks: `connect`, `WSAConnect`, `sendto`, and `WSAIoctl` retrieval of `ConnectEx`.

- [ ] **Step 1: Write local listeners and denied external fixture tests**

The test creates local TCP/UDP listeners, verifies authenticated loopback exchange, then asks the fixture to call non-loopback TCP, UDP, and dynamically obtained `ConnectEx`. The expected result is `WSAEACCES` before any packet leaves the fixture.

- [ ] **Step 2: Run the focused native integration test**

Run: `ctest --preset windows-x64-debug --output-on-failure -R WinsockHookIntegrationTests`

Expected: FAIL because socket adapters are absent.

- [ ] **Step 3: Install all required socket hooks atomically**

```cpp
if (decision.kind == NetworkDecisionKind::Deny) {
    WSASetLastError(WSAEACCES);
    ReportDenied(NetworkLayer::Winsock, operation);
    return SOCKET_ERROR;
}
```

Wrap the `ConnectEx` function pointer returned by `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`. If any required target cannot be found or enabled, remove the entire Winsock group and fail initialization.

- [ ] **Step 4: Run 50 repeated network fixtures**

Run: `1..50 | ForEach-Object { ctest --preset windows-x64-debug --output-on-failure -R WinsockHookIntegrationTests }`

Expected: 50 PASS runs; denied counter equals attempts; local listener receives only authenticated loopback traffic.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/network native/fixtures/NetworkClientFixture.cpp native/tests/WinsockHookIntegrationTests.cpp native/include/DSRRandomizer/ProtectionProtocol.h native/runtime/ProtectionBootstrap.cpp
git commit -m "feat: block non-loopback winsock traffic"
```

### Task 3: Steam interface policy and deferred module gate

**Files:**
- Create: `native/runtime/steam/SteamPolicy.h`
- Create: `native/runtime/steam/SteamPolicy.cpp`
- Create: `native/runtime/steam/SteamHooks.h`
- Create: `native/runtime/steam/SteamHooks.cpp`
- Create: `native/runtime/modules/DeferredModuleGate.h`
- Create: `native/runtime/modules/DeferredModuleGate.cpp`
- Create: `native/fixtures/FakeSteamApi.cpp`
- Create: `native/tests/SteamGuardTests.cpp`
- Modify: `native/runtime/ProtectionBootstrap.cpp`

**Interfaces:**
- Adds: `ProtectionFlags::SteamInterfaces` and `ProtectionFlags::DeferredModuleGate`.
- Produces: interface-version allowlist and method decisions for Matchmaking, Networking/P2P, and Remote Storage.

- [ ] **Step 1: Write synthetic Steam interface tests**

```cpp
REQUIRE(Policy("SteamMatchMaking009", CreateLobby) == Deny);
REQUIRE(Policy("SteamNetworking006", SendP2PPacket) == Deny);
REQUIRE(Policy("STEAMREMOTESTORAGE_INTERFACE_VERSION016", FileWrite) == Deny);
REQUIRE(Policy("SteamUser023", GetSteamID) == Allow);
REQUIRE(Policy("SteamMatchMaking999", CreateLobby) == UnknownInterfaceFatal);
```

Test a fake Steam DLL loaded after guard initialization: its protected factory result must not reach the caller before wrapper installation.

- [ ] **Step 2: Run CTest to verify failure**

Run: `ctest --preset windows-x64-debug --output-on-failure -R SteamGuardTests`

Expected: compile or assertion FAIL because Steam policy/gating is absent.

- [ ] **Step 3: Implement versioned wrappers and admission**

Gate module-load and symbol-resolution returns by canonical module path, expected SHA-256, and declared interface versions. Wrap only protected interfaces; allow ownership/user identity calls needed for `SteamAPI_Init`. A factory request for an unknown protected version sends fatal code `STEAM_INTERFACE_UNSUPPORTED` and never returns the raw interface.

```cpp
void* GuardedSteamFactory(const char* version) {
    const auto decision = steamPolicy.EvaluateInterface(version);
    if (decision == InterfaceDecision::UnknownProtectedFatal) return FatalAndNull("STEAM_INTERFACE_UNSUPPORTED");
    return decision == InterfaceDecision::Wrap ? wrappers.Get(version) : originalFactory(version);
}
```

- [ ] **Step 4: Run eager/deferred/unknown module tests**

Run: `ctest --preset windows-x64-debug --output-on-failure -R SteamGuardTests`

Expected: eager and deferred known interfaces are denied correctly; wrong-path, wrong-hash, and unknown-version cases terminate the fixture before a protected method returns.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/steam native/runtime/modules native/fixtures/FakeSteamApi.cpp native/tests/SteamGuardTests.cpp native/runtime/ProtectionBootstrap.cpp
git commit -m "feat: gate steam online interfaces"
```

### Task 4: Profile inspector and game-service target verification

**Files:**
- Create: `tools/DSRRandomizer.ProfileInspector/DSRRandomizer.ProfileInspector.csproj`
- Create: `tools/DSRRandomizer.ProfileInspector/Program.cs`
- Create: `src/DSRRandomizer.Foundation/Safety/InternalTargetProfile.cs`
- Modify: `src/DSRRandomizer.Foundation/Safety/CompatibilityProfile.cs`
- Modify: `src/DSRRandomizer.Foundation/Safety/CompatibilityProfileCatalog.cs`
- Create: `config/compatibility-profiles.json`
- Create: `native/runtime/game/GameServiceGuard.h`
- Create: `native/runtime/game/GameServiceGuard.cpp`
- Create: `native/tests/GameServiceGuardTests.cpp`
- Test: `tests/DSRRandomizer.Foundation.Tests/Safety/ProfileInspectorTests.cs`

**Interfaces:**
- Produces: read-only command `DSRRandomizer.ProfileInspector.exe verify "C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe" config\compatibility-profiles.json`.
- Extends profile with `module`, `rva`, `fingerprintSha256`, `patchLength`, and semantic action `ForceOffline` or `DenyCall`.
- Adds: `ProtectionFlags::GameServiceOffline`.

- [ ] **Step 1: Write a synthetic PE/profile validation test**

```csharp
[Fact]
public void Verify_RejectsOneByteFingerprintChange()
{
    var image = SyntheticPe.WithTarget(rva: 0x1200, bytes: TargetBytes);
    var profile = Profile.ForHash(image.Sha256).WithFingerprint(0x1200, Sha256(TargetBytes));
    image.Bytes[0x1200] ^= 1;
    Assert.Equal(ProfileError.TargetFingerprintMismatch, Inspector.Verify(image, profile).Error);
}
```

- [ ] **Step 2: Run focused managed/native tests**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter ProfileInspectorTests`

Run: `ctest --preset windows-x64-debug -R GameServiceGuardTests --output-on-failure`

Expected: FAIL because profile targets and guard do not exist.

- [ ] **Step 3: Implement read-only PE verification and derive the supported profile**

The tool memory-maps the executable and copied `steam_api64.dll` read-only, validates PE64 sections/RVA bounds, hashes whole files and declared target windows, and emits no game bytes. Use static disassembly and cross-references from the supported executable to identify the offline-state setter and login/discovery/session entry points. Record only module identities, RVAs, patch lengths, and SHA-256 fingerprints in `config/compatibility-profiles.json`; keep disassembly captures under ignored `compatibility-captures/` and never commit them. Update `CompatibilityProfileCatalog` to load this release-pinned JSON and reject duplicate or schema-mismatched profiles instead of retaining a second hard-coded source of truth. In the same red-green cycle, validate every target first, queue the offline-state and denial detours as one group, and roll back with `GAME_SERVICE_PROFILE_MISMATCH` or `GAME_SERVICE_HOOK_FAILED` before resume.

```json
{"schemaVersion":1,"profiles":[{"id":"dsr-steam-a45aaa36","modules":[],"gameServiceTargets":[]}]}
```

```cpp
for (const auto& target : profile.gameServiceTargets) {
    if (!VerifyFingerprint(image, target)) return InitStatus::GameServiceProfileMismatch;
}
return InstallGameServiceGroup(profile.gameServiceTargets);
```

Run: `dotnet run --project tools/DSRRandomizer.ProfileInspector -- verify "C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe" config/compatibility-profiles.json`

Expected: exit `0`, exact executable hash/length/machine match, and every declared target fingerprint matches. The tool performs no write to the executable or its directory.

- [ ] **Step 4: Run profile and game-service tests**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter ProfileInspectorTests`

Run: `ctest --preset windows-x64-debug -R GameServiceGuardTests --output-on-failure`

Expected: the synthetic image passes only with exact fingerprints; rollback tests leave zero installed game-service hooks.

- [ ] **Step 5: Commit**

```powershell
git add -- tools/DSRRandomizer.ProfileInspector src/DSRRandomizer.Foundation/Safety config/compatibility-profiles.json native/runtime/game native/tests/GameServiceGuardTests.cpp tests/DSRRandomizer.Foundation.Tests/Safety/ProfileInspectorTests.cs
git commit -m "feat: pin game offline protection targets"
```

### Task 5: Heartbeat, hook integrity, and fatal supervision

**Files:**
- Create: `native/runtime/monitor/ProtectionMonitor.h`
- Create: `native/runtime/monitor/ProtectionMonitor.cpp`
- Modify: `native/runtime/ProtectionBootstrap.cpp`
- Modify: `src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs`
- Modify: `src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs`
- Create: `native/tests/ProtectionMonitorTests.cpp`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/ProtectionHeartbeatTests.cs`

**Interfaces:**
- Produces: heartbeat message `(sequence, monotonicMilliseconds, activeFlags, deniedCounters)` once per second.
- Produces: fatal events `HOOK_INTEGRITY_FAILED`, `HEARTBEAT_STOPPED`, and `PROTECTION_THREAD_FAILED`.

- [ ] **Step 1: Write fake-clock timeout and mutation tests**

```csharp
[Fact]
public async Task FiveMissedHeartbeats_TerminatesJobExactlyOnce()
{
    await Harness.AdvanceAsync(TimeSpan.FromSeconds(5.1));
    Assert.Equal(1, Harness.Process.TerminateCalls);
    Assert.Equal("HEARTBEAT_TIMEOUT", Harness.Result.ErrorCode);
}
```

- [ ] **Step 2: Run focused managed/native tests**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter ProtectionHeartbeatTests`

Run: `ctest --preset windows-x64-debug --output-on-failure -R ProtectionMonitorTests`

Expected: FAIL until monitor and timeout handling exist.

- [ ] **Step 3: Implement deterministic monitoring**

Hash expected trampoline/declared patch bytes after installation, check every second, use `QueryPerformanceCounter` for monotonic time, and send counters without local user paths. The managed side uses a cancellable timer abstraction in tests and closes the Job Object on the fifth consecutive miss or any fatal event.

```cpp
while (!stopRequested.load()) {
    if (!VerifyAllInstalledHooks()) { SendFatal("HOOK_INTEGRITY_FAILED"); return; }
    SendHeartbeat(++sequence, QueryMonotonicMilliseconds(), CurrentProtectionFlags(), DeniedCounters());
    WaitOneSecondOrStop();
}
```

- [ ] **Step 4: Run fault injection 50 times**

Run: `1..50 | ForEach-Object { dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter ProtectionHeartbeatTests --no-restore }`

Run: `ctest --preset windows-x64-release --output-on-failure -R ProtectionMonitorTests`

Expected: every injected loss/mutation kills only the fixture job exactly once.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/monitor native/runtime/ProtectionBootstrap.cpp native/tests/ProtectionMonitorTests.cpp src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs tests/DSRRandomizer.Launcher.Tests/Safety/ProtectionHeartbeatTests.cs
git commit -m "feat: terminate on protection health loss"
```

### Task 6: Complete protection bitmap gate

**Files:**
- Modify: `native/include/DSRRandomizer/ProtectionProtocol.h`
- Modify: `native/runtime/ProtectionBootstrap.cpp`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs`
- Modify: `native/tests/ProtectionBootstrapTests.cpp`

**Interfaces:**
- Required bitmap: Bootstrap, SaveKnownFolder, SaveFileIo, Winsock, SteamInterfaces, DeferredModuleGate, GameServiceOffline, Heartbeat, HookIntegrity.

- [ ] **Step 1: Add one missing-flag test per protection**

```csharp
foreach (var flag in RequiredFlags.EachFlag())
{
    var handshake = SuccessfulHandshake with { ActiveFlags = RequiredFlags & ~(ulong)flag };
    await AssertLaunchDeniedAsync(handshake, "SAFETY_FLAGS_INCOMPLETE");
}
```

- [ ] **Step 2: Run complete native and managed tests**

Run: `ctest --preset windows-x64-debug --output-on-failure -R ProtectionBootstrapTests`

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter SafetyLaunchCoordinatorTests`

Expected: missing-flag cases fail before coordinator enforcement is updated.

- [ ] **Step 3: Require exact bitmap agreement**

Reject both missing required flags and unknown extra flags for protocol version `1`. Deferred Steam targets count only when the exact declared gate is armed; main image/save/socket/supervisor flags must be installed.

```csharp
if (handshake.ActiveFlags != request.RequiredFlags)
    return ProtectionHandshake.Failed("SAFETY_FLAGS_INCOMPLETE");
```

- [ ] **Step 4: Run the complete gate**

Run: `pwsh -File scripts/build-native.ps1 -Configuration Release -Test`

Run: `dotnet build DSR-Randomizer.sln -c Release --no-restore`

Run: `dotnet test DSR-Randomizer.sln -c Release --no-build`

Expected: zero failures/warnings and `--launch` still exits `2`.

- [ ] **Step 5: Commit**

```powershell
git add -- native/include/DSRRandomizer/ProtectionProtocol.h native/runtime/ProtectionBootstrap.cpp native/tests/ProtectionBootstrapTests.cpp tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs
git commit -m "feat: require complete online safety bitmap"
```

## Plan 3 Exit Gate

Do not start Plan 4 until every native and managed protection test passes, the static inspector verifies the exact supported executable, all fault injections terminate only fixtures, and no product path can launch the game.

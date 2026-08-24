# Native Launch Supervisor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and test the x64 native guard bootstrap, exact-build selection, suspended-process injection, and kill-on-close supervision without launching Dark Souls Remastered.

**Architecture:** A pure managed state machine depends on a narrow `IProtectedProcessPlatform` interface. The Windows implementation creates a harmless fixture suspended, assigns it to a Job Object, injects the project DLL, performs a nonce-authenticated named-pipe handshake, and resumes only on a complete protection bitmap. The real launcher remains disconnected from this internal coordinator.

**Tech Stack:** .NET 8.0.424, C# 12, Windows P/Invoke, MSVC v143 x64, CMake 3.28+, CTest, MinHook v1.3.4 commit `c3fcafdc10146beb5919319d0683e44e3c30d537`

**Spec:** `docs/superpowers/specs/2026-08-24-native-safety-runtime-design.md`

## Global Constraints

- Target only Windows x64; reject every other PE machine type.
- Supported game executable SHA-256 is `a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b`, length `50286344` bytes, PE timestamp `0x6344ca56`, and image size `52015104` bytes.
- Supported normal/dedicated save length is exactly `4326608` bytes.
- Do not start `DarkSoulsRemastered.exe` anywhere in this plan; all process tests use `DSRRandomizer.SuspendedFixture.exe`.
- Do not connect the WPF or `--launch` paths to the coordinator; public launch stays locked.
- `DllMain` may only save the module handle and call `DisableThreadLibraryCalls`.
- A failure before resume must terminate the Job Object; there is no retry inside the same child.
- Runtime writes remain below `%LOCALAPPDATA%\DSR-Randomizer`; repository tests use temporary directories.
- MinHook is fetched only at the pinned commit above and its BSD-2-Clause notice is preserved.

## File Structure

- `CMakePresets.json`: reproducible x64 configure/build/test presets.
- `cmake/MinHook.cmake`: pinned dependency declaration with disconnected-update behavior.
- `native/CMakeLists.txt`: native root and CTest registration.
- `native/include/DSRRandomizer/ProtectionProtocol.h`: ABI constants, flags, and packed handshake records.
- `native/runtime/GuardEntry.cpp`: minimal DLL entry and exported initializer.
- `native/runtime/ProtectionBootstrap.{h,cpp}`: all-or-nothing native initialization.
- `native/fixtures/SuspendedFixture.cpp`: harmless process that waits on a named event.
- `native/tests/ProtectionBootstrapTests.cpp`: native initialization tests.
- `scripts/build-native.ps1`: toolchain detection and preset runner.
- `src/DSRRandomizer.Foundation/Safety/CompatibilityProfile*.cs`: exact executable profile model and selection.
- `src/DSRRandomizer.Launcher/Safety/LaunchContracts.cs`: managed request/result and platform boundary.
- `src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs`: pure fail-closed state machine.
- `src/DSRRandomizer.Launcher/Native/*.cs`: SafeHandle, Job Object, suspended process, injection, and named-pipe implementations.
- `tests/DSRRandomizer.Foundation.Tests/Safety/CompatibilityProfileCatalogTests.cs`: exact profile tests.
- `tests/DSRRandomizer.Launcher.Tests/Safety/*Tests.cs`: state-machine and Windows fixture tests.

---

### Task 1: Native build and guard bootstrap

**Files:**
- Create: `CMakePresets.json`
- Create: `cmake/MinHook.cmake`
- Create: `native/CMakeLists.txt`
- Create: `native/include/DSRRandomizer/ProtectionProtocol.h`
- Create: `native/runtime/CMakeLists.txt`
- Create: `native/runtime/GuardEntry.cpp`
- Create: `native/runtime/ProtectionBootstrap.h`
- Create: `native/runtime/ProtectionBootstrap.cpp`
- Create: `native/fixtures/SuspendedFixture.cpp`
- Create: `native/tests/ProtectionBootstrapTests.cpp`
- Create: `scripts/build-native.ps1`
- Modify: `.gitignore`
- Modify: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Produces: `extern "C" __declspec(dllexport) uint32_t __stdcall InitializeProtection(ProtectionInitBlock*)`.
- Produces: protocol magic `0x44535252`, version `1`, and `ProtectionFlags : uint64_t`.

- [ ] **Step 1: Write the failing native bootstrap test**

```cpp
TEST_CASE(initialize_rejects_wrong_protocol_without_installing_hooks) {
    ProtectionInitBlock block{};
    block.magic = kProtectionMagic;
    block.version = 99;
    REQUIRE(InitializeForTest(&block) == InitStatus::UnsupportedProtocol);
    REQUIRE(CurrentProtectionFlags() == ProtectionFlags::None);
}
```

- [ ] **Step 2: Configure to prove the native project is absent**

Run: `cmake --preset windows-x64-debug`

Expected: FAIL because `CMakePresets.json` or `native/CMakeLists.txt` does not exist.

- [ ] **Step 3: Add the minimal pinned build and protocol**

```cmake
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
  GIT_SHALLOW TRUE
  UPDATE_DISCONNECTED TRUE)
```

```cpp
enum class ProtectionFlags : std::uint64_t { None = 0, Bootstrap = 1ULL << 0 };
struct ProtectionInitBlock { std::uint32_t magic; std::uint16_t version; std::uint16_t size; };
```

`GuardEntry.cpp` must call no MinHook function from `DllMain`. Add the MinHook license text and commit identity to `THIRD_PARTY_NOTICES.md`. Add `native/out/` and `.deps/` to `.gitignore` without weakening the existing game-file exclusions.

- [ ] **Step 4: Build and run CTest**

Run: `pwsh -File scripts/build-native.ps1 -Configuration Debug -Test`

Expected: CMake configures MSVC x64; CTest reports `ProtectionBootstrapTests` PASS; `dumpbin /headers` reports machine `8664` for the DLL and fixture.

- [ ] **Step 5: Commit**

```powershell
git add -- CMakePresets.json cmake/MinHook.cmake native scripts/build-native.ps1 .gitignore THIRD_PARTY_NOTICES.md
git commit -m "build: add pinned native guard toolchain"
```

### Task 2: Exact compatibility-profile selection

**Files:**
- Create: `src/DSRRandomizer.Foundation/Safety/CompatibilityProfile.cs`
- Create: `src/DSRRandomizer.Foundation/Safety/CompatibilityProfileCatalog.cs`
- Create: `src/DSRRandomizer.Foundation/Safety/ExecutableIdentity.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Safety/CompatibilityProfileCatalogTests.cs`

**Interfaces:**
- Produces: `ExecutableIdentity(long Length, string Sha256, ushort Machine, uint PeTimestamp, uint SizeOfImage)`.
- Produces: `CompatibilityProfileCatalog.Select(ExecutableIdentity) -> CompatibilityProfile` or `UnsupportedGameBuildException`.

- [ ] **Step 1: Write failing exact-match tests**

```csharp
[Fact]
public void Select_RejectsHashMatchWithWrongLength()
{
    var identity = new ExecutableIdentity(1, SupportedHash, 0x8664, 0x6344ca56, 52015104);
    Assert.Throws<UnsupportedGameBuildException>(() => CompatibilityProfileCatalog.Default.Select(identity));
}
```

Also test lowercase normalization, wrong hash, x86 machine, and duplicate profile identity rejection.

- [ ] **Step 2: Run the focused tests**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter CompatibilityProfileCatalogTests`

Expected: FAIL because the safety profile types do not exist.

- [ ] **Step 3: Implement the immutable catalog**

```csharp
public sealed record CompatibilityProfile(
    string Id, ExecutableIdentity Executable, long FixedSaveLength, ushort ProtocolVersion);

public sealed record ExecutableIdentity(
    long Length, string Sha256, ushort Machine, uint PeTimestamp, uint SizeOfImage)
{
    public string NormalizedSha256 => Sha256.ToLowerInvariant();
}
```

Create one default profile with ID `dsr-steam-a45aaa36`, executable length `50286344`, save length `4326608`, machine `0x8664`, timestamp `0x6344ca56`, image size `52015104`, and protocol `1`. Validate SHA-256 as exactly 64 hexadecimal characters before lookup.

- [ ] **Step 4: Run focused and full managed tests**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all existing and new tests PASS.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Safety tests/DSRRandomizer.Foundation.Tests/Safety
git commit -m "feat: require exact supported executable profile"
```

### Task 3: Pure fail-closed launch state machine

**Files:**
- Create: `src/DSRRandomizer.Launcher/Safety/LaunchContracts.cs`
- Create: `src/DSRRandomizer.Launcher/Safety/IProtectedProcessPlatform.cs`
- Create: `src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs`

**Interfaces:**
- Produces: `SafetyLaunchCoordinator.LaunchAsync(SafetyLaunchRequest, CancellationToken) -> SafetyLaunchResult`.
- Consumes: `IProtectedProcessPlatform.CreateSuspendedAsync`, then `IProtectedProcess.AssignKillOnCloseJob`, `InjectAndInitializeAsync`, `ResumeMainThread`, and `TerminateJob`.

- [ ] **Step 1: Write the resume-denial matrix**

```csharp
[Theory]
[InlineData(FailurePoint.Create)]
[InlineData(FailurePoint.AssignJob)]
[InlineData(FailurePoint.Inject)]
[InlineData(FailurePoint.Handshake)]
public async Task LaunchAsync_NeverResumesAfterAnyProtectionFailure(FailurePoint point)
{
    var platform = new RecordingPlatform(point);
    var result = await new SafetyLaunchCoordinator(platform).LaunchAsync(Request(), default);
    Assert.False(result.Started);
    Assert.Equal(0, platform.ResumeCalls);
    Assert.Equal(1, platform.TerminateCalls);
}
```

- [ ] **Step 2: Run the focused test and observe failure**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter SafetyLaunchCoordinatorTests`

Expected: FAIL because the coordinator and contracts do not exist.

- [ ] **Step 3: Implement only the explicit transition sequence**

```csharp
public sealed record ProtectionHandshake(bool Success, ulong ActiveFlags, string ErrorCode)
{
    public static ProtectionHandshake Failed(string code) => new(false, 0, code);
}
public sealed record SafetyLaunchResult(bool Started, string ErrorCode);
public sealed record SafetyLaunchRequest(
    string ExecutablePath, string WorkingDirectory, string GuardDllPath,
    CompatibilityProfile Profile, ulong RequiredFlags, bool DiagnosticMode);

public interface IProtectedProcessPlatform
{
    Task<IProtectedProcess> CreateSuspendedAsync(SafetyLaunchRequest request, CancellationToken token);
}

public interface IProtectedProcess : IAsyncDisposable
{
    int ProcessId { get; }
    void AssignKillOnCloseJob();
    Task<ProtectionHandshake> InjectAndInitializeAsync(CancellationToken token);
    uint ResumeMainThread();
    void TerminateJob();
}
```

The coordinator must use `await using` for `IProtectedProcess`, require `ActiveFlags == request.RequiredFlags`, require `ResumeMainThread()` to return previous suspend count `1`, and call `TerminateJob()` from every exceptional or mismatched path.

- [ ] **Step 4: Run the transition matrix and full tests**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all tests PASS and the recording fake shows exactly one resume only for a complete handshake.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher/Safety tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs
git commit -m "feat: add fail-closed launch state machine"
```

### Task 4: Windows Job Object and suspended fixture

**Files:**
- Create: `src/DSRRandomizer.Launcher/Native/NativeMethods.cs`
- Create: `src/DSRRandomizer.Launcher/Native/SafeJobHandle.cs`
- Create: `src/DSRRandomizer.Launcher/Native/SafeProcessHandle.cs`
- Create: `src/DSRRandomizer.Launcher/Native/WindowsProtectedProcessPlatform.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/WindowsJobObjectIntegrationTests.cs`

**Interfaces:**
- Implements: `IProtectedProcessPlatform` for Windows x64.
- Produces: an `IProtectedProcess` owning process, primary thread, and Job Object handles.

- [ ] **Step 1: Write a fixture death-on-job-close integration test**

```csharp
[Fact]
public async Task Dispose_KillsSuspendedFixtureAndItsChildTree()
{
    await using var process = await Platform.CreateSuspendedAsync(FixtureRequest(), default);
    var pid = process.ProcessId;
    await process.DisposeAsync();
    Assert.True(await ProcessProbe.WaitForExitAsync(pid, TimeSpan.FromSeconds(5)));
}
```

- [ ] **Step 2: Run it to verify missing Windows implementation**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter WindowsJobObjectIntegrationTests`

Expected: FAIL because `WindowsProtectedProcessPlatform` is absent.

- [ ] **Step 3: Implement direct Win32 creation and job assignment**

Use `CreateJobObjectW`, `SetInformationJobObject(JobObjectExtendedLimitInformation)` with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `CreateProcessW` with `CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT`, and `AssignProcessToJobObject`. Do not use `System.Diagnostics.Process.Start`. Build the command line with Windows quoting rules and pass an explicit working directory.

```csharp
const uint CreateSuspended = 0x00000004;
const uint CreateUnicodeEnvironment = 0x00000400;
const uint KillOnJobClose = 0x00002000;
```

- [ ] **Step 4: Run integration tests repeatedly**

Run: `1..20 | ForEach-Object { dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --no-restore --filter WindowsJobObjectIntegrationTests }`

Expected: 20 consecutive PASS results and no surviving fixture process.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher/Native tests/DSRRandomizer.Launcher.Tests/Safety/WindowsJobObjectIntegrationTests.cs
git commit -m "feat: supervise suspended child with job object"
```

### Task 5: Remote injection and authenticated bootstrap handshake

**Files:**
- Create: `src/DSRRandomizer.Launcher/Native/RemoteDllInjector.cs`
- Create: `src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs`
- Create: `src/DSRRandomizer.Launcher/Native/PeExportReader.cs`
- Modify: `native/runtime/GuardEntry.cpp`
- Modify: `native/runtime/ProtectionBootstrap.cpp`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/RemoteDllInjectorIntegrationTests.cs`

**Interfaces:**
- Produces: `RemoteDllInjector.InitializeAsync(IProtectedProcess, GuardConfiguration, CancellationToken) -> ProtectionHandshake`.
- Consumes: full-width module base from child-module enumeration and initializer RVA from `PeExportReader`.

- [ ] **Step 1: Write success, bad nonce, timeout, and wrong-protocol tests**

```csharp
[Fact]
public async Task InitializeAsync_ReturnsCompleteFixtureHandshakeBeforeResume()
{
    await using var child = await Platform.CreateSuspendedAsync(FixtureRequest(), default);
    var result = await Injector.InitializeAsync(child, FixtureGuardConfiguration(), default);
    Assert.True(result.Success);
    Assert.Equal((ulong)ProtectionFlags.Bootstrap, result.ActiveFlags);
    Assert.False(child.WasResumed);
}
```

- [ ] **Step 2: Run focused tests to verify failure**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter RemoteDllInjectorIntegrationTests`

Expected: FAIL because the injector and pipe server do not exist.

- [ ] **Step 3: Implement bounded injection**

Allocate/write/read-back the canonical DLL path, call remote `LoadLibraryW`, wait at most 10 seconds, enumerate modules by canonical path for the 64-bit base, parse the export RVA without loading the DLL locally, call `InitializeProtection`, and require a matching named-pipe message within 10 seconds. The pipe name contains 32 random bytes encoded as lowercase hex and its ACL permits only the current user SID and LocalSystem.

```csharp
if (handshake.ProtocolVersion != 1 || !CryptographicOperations.FixedTimeEquals(expectedNonce, handshake.Nonce))
    return ProtectionHandshake.Failed("SAFETY_IPC_AUTH_FAILED");
```

- [ ] **Step 4: Run native, managed, and leak checks**

Run: `pwsh -File scripts/build-native.ps1 -Configuration Release -Test`

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all CTest and managed tests PASS; failure tests leave no fixture process and no open named pipe.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime src/DSRRandomizer.Launcher/Native tests/DSRRandomizer.Launcher.Tests/Safety/RemoteDllInjectorIntegrationTests.cs
git commit -m "feat: inject guard before suspended child resumes"
```

### Task 6: CI gate and locked product integration

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`

**Interfaces:**
- Preserves: `Program.Main(new[] { "--launch" }) == 2`.
- Produces: CI native artifacts for later plans, but no distributable guard package yet.

- [ ] **Step 1: Add failing policy assertions**

```csharp
[Fact]
public void ProductLaunch_RemainsLockedAfterNativeFoundation()
{
    Assert.Equal(2, Program.Main(new[] { "--launch" }));
}
```

- [ ] **Step 2: Add native configure/build/test CI steps**

```yaml
- run: cmake --preset windows-x64-release
- run: cmake --build --preset windows-x64-release
- run: ctest --preset windows-x64-release --output-on-failure
```

- [ ] **Step 3: Run the complete local gate**

Run: `pwsh -File scripts/build-native.ps1 -Configuration Release -Test`

Run: `dotnet build DSR-Randomizer.sln -c Release --no-restore`

Run: `dotnet test DSR-Randomizer.sln -c Release --no-build`

Expected: native and managed tests PASS, build has zero warnings/errors, and product launch remains rejected.

- [ ] **Step 4: Inspect source/install safety**

Run: `git diff --check`

Run: `git status --short`

Expected: only plan-scoped repository paths are changed; no file exists under the Steam installation because of these tests.

- [ ] **Step 5: Commit**

```powershell
git add -- .github/workflows/ci.yml tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs
git commit -m "ci: require native supervisor tests"
```

## Plan 1 Exit Gate

Do not start Plan 2 until CTest and managed tests pass, 20 Job Object repetitions leave no fixture, `--launch` still exits `2`, and code review confirms that no real-game path is reachable.

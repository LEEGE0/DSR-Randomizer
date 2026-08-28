# Simplified Offline Mod Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a copied, external-disk Dark Souls Remastered runtime that launches with a dedicated `.rmm` and one pre-resume seven-flag offline protection check while letting the user install or delete mod folders manually.

**Architecture:** Preserve the verified copy, save redirection, suspended injection, process-scoped socket block, pinned Steam proxies, and game offline hooks. Add an explicit simplified bitmap path that closes the authenticated pipe after one handshake, relax runtime readiness only for non-core modded files, select the material data root through a small local pointer, and connect the existing coordinator to CLI/WPF launch without requiring continuous heartbeat monitoring.

**Tech Stack:** .NET 8, WPF, C++20/MSVC x64, MinHook 1.3.4, CMake/CTest, xUnit, Windows Job Objects and named pipes

**Spec:** `docs/superpowers/specs/2026-08-28-simplified-offline-mod-runtime-design.md`

## Global Constraints

- Never write to the Steam Dark Souls Remastered installation, installed Overhaul, normal `.sl2`, Steam settings, Windows Firewall, or network-adapter settings.
- Never hard-link, junction, symlink, or reparse the copied runtime back to the source installation.
- The simplified required bitmap is exactly bits 0 through 6: `0x7F`; missing flags and unknown extras fail before resume.
- `ProtectionInitBlock` remains protocol version 2 and exactly 5480 bytes.
- The simplified path does not start or require `Heartbeat` or `HookIntegrity`; the experimental monitored path may remain independently tested.
- `DarkSoulsRemastered.exe` and adjacent `steam_api64.dll` must match the release-pinned profile exactly.
- Mod presence is filesystem state only. Do not add enable/disable state, a load-order database, per-mod rollback, or randomizer generation.
- All material files live under the selected external root. Only `%LOCALAPPDATA%\DSR-Randomizer\external-root.json` may be written locally.
- Use only synthetic/harmless fixtures. Do not start the real game during this plan.
- Public launch remains locked until Task 4 completes and the whole branch passes independent review.

## Preflight Rulings

- The active content-addressed runtime selected by the existing runtime pointer is the conceptual `Game` directory. Do not create a second copy, junction, or reparse-point alias.
- `RuntimeReadinessService` remains the strict clean-copy audit. The new `ModRuntimeReadinessService` is the only readiness check used by the modded launch path.
- `RuntimeBuilder` copies only paths present in the verified stock `GameFileCatalog`; files present in the source but absent from that copy manifest are never copied. It may create an empty top-level `Mods` directory after writing the clean runtime manifest. Once the user adds files, strict audit may fail by design while mod readiness can still succeed.
- The known experimental heartbeat/monitor implementation is out of the simplified product path. Task reviews only reopen it if the new one-shot path reaches or weakens it.
- Root changes take effect after restart so every material service has one immutable external root for its lifetime.
- A launch must verify an existing valid `.rmm` without opening or mutating the normal `.sl2`; save bootstrap remains a separate explicit operation.

---

### Task 1: Exact one-shot offline protection contract

**Files:**
- Modify: `native/include/DSRRandomizer/ProtectionProtocol.h`
- Modify: `native/runtime/ProtectionBootstrap.cpp`
- Modify: `native/tests/ProtectionBootstrapTests.cpp`
- Create: `src/DSRRandomizer.Launcher/Safety/SimplifiedOfflineProtection.cs`
- Modify: `src/DSRRandomizer.Launcher/Native/RemoteDllInjector.cs`
- Modify: `src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs`
- Modify: `src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Safety/RemoteDllInjectorIntegrationTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs`

**Interfaces:**
- Produces: native `kSimplifiedOfflineRequiredFlags = 0x7F`.
- Produces: managed `SimplifiedOfflineProtection.RequiredFlags = 0x7FUL` and `IsExact(ulong)`.
- Produces: a successful simplified `ProtectionHandshake` with `Session == null`.
- Preserves: experimental monitor session only when both bits 7 and 8 are explicitly requested.

- [ ] **Step 1: Write the failing exact-bitmap and one-shot tests**

```csharp
[Fact]
public async Task LaunchAsync_ExactSimplifiedBitmap_ResumesWithoutMonitorSession()
{
    var process = new RecordingProcess(
        new ProtectionHandshake(true, 0x7F, string.Empty, Session: null));
    var result = await new SafetyLaunchCoordinator(new RecordingPlatform(process))
        .LaunchAsync(Request(requiredFlags: 0x7F), CancellationToken.None);
    Assert.True(result.Started);
    Assert.Equal(1, process.ResumeCalls);
    Assert.Equal(0, process.TerminateCalls);
}

[Theory]
[InlineData(0x7EUL)]
[InlineData(0xFFUL)]
[InlineData(0x17FUL)]
public async Task LaunchAsync_NonExactSimplifiedBitmap_NeverResumes(ulong flags)
{
    var process = new RecordingProcess(new ProtectionHandshake(true, flags, string.Empty));
    var result = await new SafetyLaunchCoordinator(new RecordingPlatform(process))
        .LaunchAsync(Request(requiredFlags: 0x7F), CancellationToken.None);
    Assert.False(result.Started);
    Assert.Equal(0, process.ResumeCalls);
    Assert.Equal(1, process.TerminateCalls);
}
```

Native coverage must table-drive all seven missing-bit cases, bit 7, bit 8, and one unknown high bit against production `InitializeProtection` using a harmless pipe fixture.

- [ ] **Step 2: Run focused tests and observe RED**

Run:

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter "SafetyLaunchCoordinatorTests|RemoteDllInjectorIntegrationTests"
& 'C:\Program Files\CMake\bin\ctest.exe' --preset windows-x64-debug -R ProtectionBootstrapTests --output-on-failure
```

Expected: tests fail because production native requires the monitor pair and the injector/coordinator always transfer and require a session.

- [ ] **Step 3: Implement the exact dual path**

```cpp
inline constexpr std::uint64_t kSimplifiedOfflineRequiredFlags =
    (1ULL << 7) - 1ULL;

if (block->requiredFlags == kSimplifiedOfflineRequiredFlags) {
    const auto status = InitializeCore(block, &ReadRequiredPath, nullptr);
    if (status != InitStatus::Success) return status;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto report = OpenSupervisorSession(*block, pipe);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    return report;
}
```

The existing monitored path remains reachable only for the exact documented monitored bitmap. Managed injection disposes the pipe after the simplified handshake and returns `Session = null`; coordinator accepts null only when the request and handshake are both exactly `0x7F`.

- [ ] **Step 4: Run focused and full Debug/Release tests**

Run the focused commands above, then:

```powershell
dotnet test DSR-Randomizer.sln -c Debug
dotnet test DSR-Randomizer.sln -c Release
& 'C:\Program Files\CMake\bin\cmake.exe' --build --preset windows-x64-debug --config Debug
& 'C:\Program Files\CMake\bin\ctest.exe' --preset windows-x64-debug --output-on-failure
& 'C:\Program Files\CMake\bin\cmake.exe' --build --preset windows-x64-release --config Release
& 'C:\Program Files\CMake\bin\ctest.exe' --preset windows-x64-release --output-on-failure
```

Expected: all pass; the native protocol layout assertion remains 5480 bytes.

- [ ] **Step 5: Commit**

```powershell
git add -- native/include/DSRRandomizer/ProtectionProtocol.h native/runtime/ProtectionBootstrap.cpp native/tests/ProtectionBootstrapTests.cpp src/DSRRandomizer.Launcher/Safety/SimplifiedOfflineProtection.cs src/DSRRandomizer.Launcher/Native/RemoteDllInjector.cs src/DSRRandomizer.Launcher/Native/ProtectionPipeServer.cs src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs tests/DSRRandomizer.Launcher.Tests/Safety/RemoteDllInjectorIntegrationTests.cs tests/DSRRandomizer.Launcher.Tests/Safety/SafetyLaunchCoordinatorTests.cs
git commit -m "feat: add simplified offline protection contract"
```

### Task 2: Mod-ready copied runtime validation

**Files:**
- Create: `src/DSRRandomizer.Foundation/Runtime/ModRuntimeReadinessService.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/ModFolderDiscovery.cs`
- Modify: `src/DSRRandomizer.Foundation/Runtime/RuntimeBuilder.cs`
- Modify: `src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Runtime/ModRuntimeReadinessServiceTests.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Runtime/ModFolderDiscoveryTests.cs`

**Interfaces:**
- Consumes: existing `RuntimePointer`, `RuntimeManifest`, `WriteBoundary`, and `FileHashService`.
- Produces: `ModRuntimeReadinessService.ValidateAsync(CancellationToken)`.
- Produces: `ModFolderDiscovery.Discover(string runtimePath)` returning sorted top-level folder names only.
- Preserves: existing strict `RuntimeReadinessService` for immutable-copy audits.

- [ ] **Step 1: Write failing mod-readiness tests**

```csharp
[Fact]
public async Task ValidateAsync_AllowsOrdinaryDataReplacementAndNewModFolder()
{
    using var fixture = await ModRuntimeFixture.CreateAsync();
    fixture.ReplaceOrdinaryFile("map/data.bin", "modded");
    fixture.AddFile("Mods/EnemyRandomizer/config.ini", "enabled=1");
    var result = await fixture.Service.ValidateAsync(CancellationToken.None);
    Assert.True(result.IsReady);
}

[Theory]
[InlineData("DarkSoulsRemastered.exe")]
[InlineData("steam_api64.dll")]
public async Task ValidateAsync_RejectsProtectedCoreChange(string relativePath)
{
    using var fixture = await ModRuntimeFixture.CreateAsync();
    fixture.ReplaceOrdinaryFile(relativePath, "changed");
    var result = await fixture.Service.ValidateAsync(CancellationToken.None);
    Assert.False(result.IsReady);
}
```

Add cases for a reparse point anywhere, runtime outside the selected root, missing/changed manifest, case-colliding protected names, top-level mod folder sorting, and folder deletion removing the name without persisted state.

Add a `RuntimeBuilderTests.BuildAsync_CopiesOnlyVerifiedCatalogEntries` characterization test. Place an extra file under a stock source directory but omit it from the verified `GameFileCatalog`; assert the file is absent from the built runtime and the generated runtime manifest. `RuntimeBuilder` must never enumerate unlisted source files as copy candidates.

- [ ] **Step 2: Run focused tests and observe RED**

Run:

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter "ModRuntimeReadinessServiceTests|ModFolderDiscoveryTests"
```

Expected: compile failure because the two services do not exist.

- [ ] **Step 3: Implement protected-core-only readiness**

`ModRuntimeReadinessService` must validate the active pointer and manifest exactly as the strict service does, then hash only these manifest-backed protected core files:

```csharp
private static readonly string[] ProtectedCore =
[
    "DarkSoulsRemastered.exe",
    "steam_api64.dll"
];
```

It must require every traversed path to remain a non-reparse descendant, allow ordinary file changes/additions/deletions, and never modify the runtime. `RuntimeBuilder` copies only the verified stock catalog entries and creates an empty `Mods` directory after activating the verified runtime. It must not copy a source file that is absent from the catalog, even when that file is located under a stock data directory. `ModFolderDiscovery` rejects reparse directories and returns only `DirectoryInfo.Name`, sorted ordinal-ignore-case.

- [ ] **Step 4: Run focused and full Foundation tests**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter "ModRuntimeReadinessServiceTests|ModFolderDiscoveryTests"
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj -c Debug
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj -c Release
```

Expected: all pass; existing strict immutability tests remain unchanged and green.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Runtime/ModRuntimeReadinessService.cs src/DSRRandomizer.Foundation/Runtime/ModFolderDiscovery.cs src/DSRRandomizer.Foundation/Runtime/RuntimeBuilder.cs src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs tests/DSRRandomizer.Foundation.Tests/Runtime/ModRuntimeReadinessServiceTests.cs tests/DSRRandomizer.Foundation.Tests/Runtime/ModFolderDiscoveryTests.cs tests/DSRRandomizer.Foundation.Tests/Runtime/RuntimeBuilderTests.cs
git commit -m "feat: allow folder-based mods in copied runtime"
```

### Task 3: External-root selection

**Files:**
- Create: `src/DSRRandomizer.Launcher/Configuration/ExternalRootSelectionStore.cs`
- Modify: `src/DSRRandomizer.Launcher/Program.cs`
- Modify: `src/DSRRandomizer.Launcher/App.xaml.cs`
- Modify: `src/DSRRandomizer.Launcher/LauncherApplication.cs`
- Modify: `src/DSRRandomizer.Launcher/MainWindow.xaml`
- Modify: `src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs`
- Create: `tests/DSRRandomizer.Launcher.Tests/Configuration/ExternalRootSelectionStoreTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`

**Interfaces:**
- Produces: schema-1 `%LOCALAPPDATA%\DSR-Randomizer\external-root.json` containing one canonical absolute `root`.
- Produces CLI: `--set-root "D:\DSR-Modded"`.
- Produces WPF display of the selected material root; changing it records the pointer and requires application restart before material operations.
- Consumes: `LauncherService(selectedExternalRoot)`; no game/mod/save content is written beside the pointer.

- [ ] **Step 1: Write failing pointer-boundary tests**

```csharp
[Fact]
public async Task WriteAsync_StoresOnlyCanonicalExternalRoot()
{
    var store = Fixture.Store;
    await store.WriteAsync(@"D:\DSR-Modded\", CancellationToken.None);
    Assert.Equal(@"D:\DSR-Modded", await store.ReadAsync(CancellationToken.None));
    Assert.Equal(new[] { "external-root.json" }, Fixture.LocalFiles());
}
```

Add rejection for relative paths, source-installation descendants, reparse roots, filesystem root itself, malformed/duplicate/unknown JSON fields, and a local pointer containing a nonexistent external root.

- [ ] **Step 2: Run focused tests and observe RED**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter "ExternalRootSelectionStoreTests|LauncherApplicationTests|MainWindowViewModelTests"
```

Expected: compile/test failure because no root store or command exists.

- [ ] **Step 3: Implement root selection and bootstrap**

The pointer schema is exact and duplicate/unknown fields fail:

```json
{"schemaVersion":1,"root":"D:\\DSR-Modded"}
```

`Program.Main` handles `--set-root` before constructing `LauncherService`; other CLI/WPF startup reads the pointer and refuses material operations with `EXTERNAL_ROOT_NOT_SELECTED` when absent. WPF shows the current root and offers a bounded “Save external root” command; after a successful change its status explicitly says restart is required. Existing source installation selection remains read-only under the external root's `config` directory after restart.

- [ ] **Step 4: Run focused and full managed tests**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter "ExternalRootSelectionStoreTests|LauncherApplicationTests|MainWindowViewModelTests"
dotnet test DSR-Randomizer.sln -c Debug
dotnet test DSR-Randomizer.sln -c Release
```

Expected: all pass and tests prove the local pointer is the only local material-root bootstrap write.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher/Configuration/ExternalRootSelectionStore.cs src/DSRRandomizer.Launcher/Program.cs src/DSRRandomizer.Launcher/App.xaml.cs src/DSRRandomizer.Launcher/LauncherApplication.cs src/DSRRandomizer.Launcher/MainWindow.xaml src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs tests/DSRRandomizer.Launcher.Tests/Configuration/ExternalRootSelectionStoreTests.cs tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs
git commit -m "feat: select external mod runtime root"
```

### Task 4: Connect the simplified modded launch path

**Files:**
- Modify: `src/DSRRandomizer.Launcher/Safety/LaunchContracts.cs`
- Modify: `src/DSRRandomizer.Launcher/Native/WindowsProtectedProcessPlatform.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/ILauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/LauncherApplication.cs`
- Modify: `src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs`
- Modify: `src/DSRRandomizer.Launcher/MainWindow.xaml`
- Modify: `src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Services/LauncherServiceTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Safety/WindowsJobObjectIntegrationTests.cs`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Produces: `ILauncherService.LaunchModdedAsync(string steamId, CancellationToken)`.
- Produces CLI: `--launch <SteamID>` using the selected external root.
- Produces WPF `LaunchCommand` enabled only after mod-ready runtime and valid existing `.rmm` checks.
- Consumes: exact `0x7F` contract, pinned profile catalog, `ModRuntimeReadinessService`, existing dedicated save service, and `WindowsProtectedProcessPlatform`.

- [ ] **Step 1: Write failing end-to-end fixture tests**

```csharp
[Fact]
public async Task LaunchModdedAsync_UsesCopiedExeDedicatedRmmAndExactBitmap()
{
    var fixture = await LauncherFixture.CreateReadyAsync();
    var result = await fixture.Service.LaunchModdedAsync(
        fixture.SteamId,
        CancellationToken.None);
    Assert.True(result.Started);
    Assert.Equal(fixture.RuntimeExe, fixture.Platform.Request.ExecutablePath);
    Assert.Equal(fixture.DedicatedRmm, fixture.Platform.Request.SavePaths!.DedicatedRmm);
    Assert.Equal(0x7FUL, fixture.Platform.Request.RequiredProtectionFlags);
    Assert.Equal(1, fixture.Platform.Process.ResumeCalls);
}
```

Add failures for missing `.rmm`, changed core EXE/DLL, runtime/source path equality, reparse escape, unsupported profile, missing guard DLL, incomplete handshake, unexpected session on the simplified path, and attempted normal/Overhaul save path use.

- [ ] **Step 2: Run focused tests and observe RED**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter "LauncherServiceTests|LauncherApplicationTests|MainWindowViewModelTests|WindowsJobObjectIntegrationTests"
```

Expected: compile/test failure because the service and product commands remain locked.

- [ ] **Step 3: Implement launch request construction**

`LauncherService` performs this order without opening the normal save:

```text
mod-ready runtime validation
-> exact runtime EXE identity and compatibility profile selection
-> existing valid DRAKS0005.rmm verification
-> packaged guard DLL presence/hash policy
-> construct virtual Documents and dedicated save paths under external root
-> SafetyLaunchCoordinator.LaunchAsync with RequiredFlags = 0x7F
```

`WindowsProtectedProcessPlatform` copies the request's exact save paths into `GuardConfiguration`; socket endpoint count is zero, which denies all game TCP/UDP while named-pipe supervision remains available. CLI and WPF call the same service. The WPF warning states “modded copy only; original and Overhaul remain untouched,” and its launch button never targets the source installation.

- [ ] **Step 4: Add packaging and release-content assertions**

The launcher project copies the Release native guard and `config/compatibility-profiles.json` into deterministic package locations. Tests require both and continue rejecting game binaries, game data, saves, local mod folders, captures, credentials, and locally generated profiles from release archives.

- [ ] **Step 5: Run the complete gate**

```powershell
dotnet test DSR-Randomizer.sln -c Debug
dotnet test DSR-Randomizer.sln -c Release
& 'C:\Program Files\CMake\bin\cmake.exe' --build --preset windows-x64-debug --config Debug
& 'C:\Program Files\CMake\bin\ctest.exe' --preset windows-x64-debug --output-on-failure
& 'C:\Program Files\CMake\bin\cmake.exe' --build --preset windows-x64-release --config Release
& 'C:\Program Files\CMake\bin\ctest.exe' --preset windows-x64-release --output-on-failure
git diff --check
```

Expected: all pass. No command in this task starts the real game; only harmless fixtures run.

- [ ] **Step 6: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher/LaunchContracts.cs src/DSRRandomizer.Launcher/WindowsProtectedProcessPlatform.cs src/DSRRandomizer.Launcher/LauncherService.cs src/DSRRandomizer.Launcher/ILauncherService.cs src/DSRRandomizer.Launcher/LauncherApplication.cs src/DSRRandomizer.Launcher/MainWindow.xaml src/DSRRandomizer.Launcher/MainWindow.xaml.cs src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj tests/DSRRandomizer.Launcher.Tests README.md CHANGELOG.md
git commit -m "feat: launch isolated offline mod runtime"
```

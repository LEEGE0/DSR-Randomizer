# Dedicated RMM Save Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create and validate `DRAKS0005.rmm` from a read-only normal save, then redirect the guarded child to that external file while denying normal and Overhaul save access.

**Architecture:** Managed code owns profile selection, atomic bootstrap, metadata, and recovery. A pure native `SavePathPolicy` decides allow/redirect/deny before hook adapters touch Windows APIs. Existing valid `.rmm` validation reads only external metadata and data; it does not reopen the normal `.sl2`.

**Tech Stack:** .NET 8/C# 12, System.Text.Json, Windows Known Folder API, C++20, MinHook, xUnit, CTest

**Spec:** `docs/superpowers/specs/2026-08-24-native-safety-runtime-design.md`

## Global Constraints

- Physical target: `%LOCALAPPDATA%\DSR-Randomizer\saves\<SteamID>\DRAKS0005.rmm`.
- The first exact compatibility profile accepts save length `4326608` bytes; any other length blocks preparation.
- Discover Documents with `FOLDERID_Documents`; do not assume `%USERPROFILE%\Documents`.
- The production launcher and copied game never open an Overhaul save.
- First bootstrap opens exact `DRAKS0005.sl2` for read only; no write/delete sharing or attribute mutation.
- Valid existing `.rmm` takes precedence and causes zero normal-save file opens.
- Invalid or mismatched `.rmm` blocks; it is never silently replaced.
- Every staged write and archive remains beneath the canonical external saves/staging roots.
- This plan uses only fake/fixture game processes; public launch remains locked.

## File Structure

- `src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs`: add components and virtual-profile roots.
- `src/DSRRandomizer.Foundation/Saves/SaveContracts.cs`: candidates, metadata, readiness, and error codes.
- `src/DSRRandomizer.Foundation/Saves/WindowsSaveProfileLocator.cs`: numeric SteamID discovery via Known Folder API.
- `src/DSRRandomizer.Foundation/Saves/SaveSelectionStore.cs`: external selected-profile persistence.
- `src/DSRRandomizer.Foundation/Saves/DedicatedSaveService.cs`: reuse/bootstrap/reset transaction.
- `src/DSRRandomizer.Foundation/Saves/IFileAccess.cs`: testable exact access-mode boundary.
- `native/runtime/save/SavePathPolicy.{h,cpp}`: pure logical-path decisions.
- `native/runtime/save/SaveHooks.{h,cpp}`: Win32 and known-folder adapters.
- `native/tests/SavePathPolicyTests.cpp`: exhaustive path matrix.
- `src/DSRRandomizer.Launcher/ViewModels/SaveProfileViewModel.cs`: explicit selection and prepare command.
- `tests/**/Saves/*Tests.cs`: no-source-open and source-immutability tests.

---

### Task 1: External layout and save contracts

**Files:**
- Modify: `src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/SaveContracts.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Paths/WriteBoundaryTests.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Saves/SaveContractsTests.cs`

**Interfaces:**
- Produces: `LocalDataLayout.Components`, `LocalDataLayout.VirtualProfile`.
- Produces: `SaveProfileCandidate`, `DedicatedSaveMetadata`, `DedicatedSaveResult`, `SaveErrorCode`.

- [ ] **Step 1: Write failing layout and metadata tests**

```csharp
[Fact]
public void DedicatedPath_IsNamespacedByNumericSteamId()
{
    var path = SavePaths.GetDedicatedSave(@"C:\Local\DSR", "12345678901234567");
    Assert.Equal(@"C:\Local\DSR\saves\12345678901234567\DRAKS0005.rmm", path);
    Assert.Throws<ArgumentException>(() => SavePaths.GetDedicatedSave(@"C:\Local\DSR", "../escape"));
}
```

- [ ] **Step 2: Run focused tests**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter "SaveContractsTests|LocalDataLayout"`

Expected: FAIL because the save contracts and new layout properties are absent.

- [ ] **Step 3: Implement strict value objects**

```csharp
public sealed record SaveProfileCandidate(string SteamId, string SourcePath);
public sealed record DedicatedSaveMetadata(
    int SchemaVersion, string SteamId, long FixedLength, string LastKnownSha256,
    string? ActiveSeedId, string? PlacementSha256, bool CleanExit);
public sealed record SeedBinding(string SeedId, string PlacementSha256);
public sealed record DedicatedSaveResult(
    bool Ready, bool ReusedExisting, string? SavePath, SaveErrorCode ErrorCode, string Message)
{
    public static DedicatedSaveResult Fail(SaveErrorCode code, string message = "") =>
        new(false, false, null, code, message);
}
public enum SaveErrorCode
{
    None,
    InvalidSteamId,
    SourceMissing,
    MultipleProfilesRequireSelection,
    ExistingSaveInvalid,
    CopyVerificationFailed,
    SourceChanged,
    DestinationRace,
    SeedMismatch,
    PathDenied
}
```

`SavePaths` accepts only `^[0-9]{16,20}$`, canonicalizes the destination, and calls `WriteBoundary.EnsureAllowed` before returning it.

- [ ] **Step 4: Run focused and full tests**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all tests PASS; prior layout expectations now include `components` and `profile` in exact order.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs src/DSRRandomizer.Foundation/Saves tests/DSRRandomizer.Foundation.Tests/Paths/WriteBoundaryTests.cs tests/DSRRandomizer.Foundation.Tests/Saves/SaveContractsTests.cs
git commit -m "feat: define external rmm save layout"
```

### Task 2: Profile discovery and explicit selection

**Files:**
- Create: `src/DSRRandomizer.Foundation/Saves/IKnownFolderProvider.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/WindowsKnownFolderProvider.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/WindowsSaveProfileLocator.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/SaveSelectionStore.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Saves/WindowsSaveProfileLocatorTests.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Saves/SaveSelectionStoreTests.cs`

**Interfaces:**
- Produces: `ISaveProfileLocator.DiscoverAsync(CancellationToken) -> IReadOnlyList<SaveProfileCandidate>`.
- Produces: `SaveSelectionStore.ReadAsync/WriteAsync` storing only SteamID and canonical exact source path externally.

- [ ] **Step 1: Write discovery exclusion tests**

```csharp
[Fact]
public async Task DiscoverAsync_ReturnsOnlyNumericExactNormalSave()
{
    Fixture.File("12345678901234567/DRAKS0005.sl2");
    Fixture.File("12345678901234567/DRAKS0005.sl2.overhaul.sl2");
    Fixture.File("backup/DRAKS0005.sl2");
    var result = await Fixture.Locator.DiscoverAsync(default);
    Assert.Collection(result, x => Assert.Equal("12345678901234567", x.SteamId));
}
```

- [ ] **Step 2: Run the focused tests**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter "WindowsSaveProfileLocatorTests|SaveSelectionStoreTests"`

Expected: FAIL because the locator/store are absent.

- [ ] **Step 3: Implement Known Folder discovery without file-content reads**

Use `SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT)` and enumerate only immediate numeric directories beneath `NBGI\DARK SOULS REMASTERED`. Match the leaf with `StringComparison.OrdinalIgnoreCase` to exactly `DRAKS0005.sl2`; never use a wildcard that includes suffixes.

```csharp
public interface ISaveProfileLocator
{
    Task<IReadOnlyList<SaveProfileCandidate>> DiscoverAsync(CancellationToken cancellationToken);
}
```

- [ ] **Step 4: Test zero/one/multiple profile behavior**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter Saves`

Expected: zero returns an empty list, one returns one sorted candidate, multiple returns sorted candidates without automatic selection.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Saves tests/DSRRandomizer.Foundation.Tests/Saves
git commit -m "feat: discover exact normal save profiles"
```

### Task 3: Atomic bootstrap and existing-RMM reuse

**Files:**
- Create: `src/DSRRandomizer.Foundation/Saves/IFileAccess.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/SystemFileAccess.cs`
- Create: `src/DSRRandomizer.Foundation/Saves/DedicatedSaveService.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Saves/DedicatedSaveServiceTests.cs`

**Interfaces:**
- Produces: `DedicatedSaveService.PrepareAsync(string steamId, CancellationToken) -> DedicatedSaveResult`.
- Produces: `DedicatedSaveService.ResetForSeedAsync(string steamId, SeedBinding, CancellationToken)` for the later seed subsystem.
- Produces: `DedicatedSaveService.BeginSessionAsync/CompleteSessionAsync` to mark unclean-before-resume and hash/mark-clean after normal exit.

- [ ] **Step 1: Write the no-source-open and read-only bootstrap tests**

```csharp
[Fact]
public async Task PrepareAsync_ValidExistingRmm_NeverOpensNormalSave()
{
    Fixture.CreateValidExternalRmm();
    var result = await Fixture.Service.PrepareAsync(Fixture.SteamId, default);
    Assert.True(result.Ready);
    Assert.DoesNotContain(Fixture.Access.Opens, x => x.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
}
```

Add tests for source open access `FileAccess.Read`, no `FileShare.Delete`, concurrent source mutation, short copy, staged hash mismatch, destination race, invalid metadata, reparse escape, and seed-reset rollback. Add session tests proving `CleanExit=false` is persisted before resume and `CleanExit=true` plus the new `.rmm` hash is written only after a normal guarded exit.

- [ ] **Step 2: Run focused tests to prove failure**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter DedicatedSaveServiceTests`

Expected: FAIL because `DedicatedSaveService` is absent.

- [ ] **Step 3: Implement the transaction exactly**

```csharp
await using var source = _files.Open(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read);
var before = await _files.IdentityAndHashAsync(source, token);
await _files.CopyAndFlushAsync(source, stagedPath, token);
var staged = await _files.IdentityAndHashAsync(stagedPath, token);
if (before.Length != staged.Length || before.Sha256 != staged.Sha256)
    return DedicatedSaveResult.Fail(SaveErrorCode.CopyVerificationFailed);
```

After rechecking source identity/length/write time, use a create-new destination operation so a race cannot overwrite an existing `.rmm`. Write metadata to a unique external temporary file, flush, then atomic-replace only the external metadata. On any failure delete only the unique staged files.

- [ ] **Step 4: Run tests and capture source before/after**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter DedicatedSaveServiceTests`

Expected: all bootstrap/reuse/race tests PASS and source content, length, timestamps, and attributes remain equal.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Saves tests/DSRRandomizer.Foundation.Tests/Saves/DedicatedSaveServiceTests.cs
git commit -m "feat: bootstrap dedicated rmm transactionally"
```

### Task 4: Pure native save-path policy

**Files:**
- Create: `native/runtime/save/SavePathPolicy.h`
- Create: `native/runtime/save/SavePathPolicy.cpp`
- Create: `native/tests/SavePathPolicyTests.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Produces: `PathDecision EvaluateSavePath(PathOperation, std::wstring_view)`.
- Produces: `PathDecisionKind::{Allow, Redirect, Deny}` and canonical redirect target.

- [ ] **Step 1: Write an exhaustive table-driven policy test**

```cpp
const Case cases[] = {
  {L"C:\\Virtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2", Redirect},
  {L"C:\\Users\\U\\Documents\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2", Deny},
  {L"C:\\Users\\U\\Documents\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2.overhaul.sl2", Deny},
  {L"C:\\External\\unrelated.txt", Allow},
};
```

Include mixed case, `/`, `..`, short-name/reparse-resolved input, wrong SteamID, backup suffix, rename source/destination, delete, and enumeration cases.

- [ ] **Step 2: Run CTest and observe missing policy**

Run: `pwsh -File scripts/build-native.ps1 -Configuration Debug -Test`

Expected: native compile FAIL because `SavePathPolicy` is absent.

- [ ] **Step 3: Implement normalize-then-decide**

The policy receives already resolved DOS paths from a canonicalizer adapter, compares case-insensitively by path segment, redirects only the selected logical normal save to `DRAKS0005.rmm`, and denies the real save root and every Overhaul suffix before generic allow rules.

```cpp
PathDecision SavePathPolicy::Evaluate(PathOperation operation, std::wstring_view canonicalPath) const {
    if (IsBelow(canonicalPath, realSaveRoot_) || IsOverhaulSave(canonicalPath)) return Deny();
    if (IsSelectedLogicalSave(canonicalPath)) return RedirectTo(dedicatedRmm_);
    return Allow();
}
```

- [ ] **Step 4: Run native tests**

Run: `ctest --preset windows-x64-debug --output-on-failure`

Expected: all path cases PASS; no test touches a real Documents directory.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/save native/tests/SavePathPolicyTests.cpp native/CMakeLists.txt
git commit -m "feat: enforce dedicated save path policy"
```

### Task 5: Hook adapters and fixture file operations

**Files:**
- Create: `native/runtime/save/SaveHooks.h`
- Create: `native/runtime/save/SaveHooks.cpp`
- Create: `native/fixtures/SaveClientFixture.cpp`
- Create: `native/tests/SaveHookIntegrationTests.cpp`
- Modify: `native/runtime/ProtectionBootstrap.cpp`
- Modify: `native/include/DSRRandomizer/ProtectionProtocol.h`

**Interfaces:**
- Adds: `ProtectionFlags::SaveKnownFolder` and `ProtectionFlags::SaveFileIo`.
- Hooks: known-folder resolution and the profiled file APIs through `SavePathPolicy`.
- Produces: redacted save-operation audit events classified as `DedicatedRmm`, `DeniedNormal`, `DeniedOverhaul`, or `Unrelated`.

- [ ] **Step 1: Write fixture integration failures first**

The fixture requests the virtual `.sl2`, writes a sentinel, performs flush/rename/attribute/enumeration operations, then attempts normal and Overhaul paths. Assert the sentinel exists only in external `.rmm` and both prohibited attempts return access denied.

- [ ] **Step 2: Run native integration tests**

Run: `ctest --preset windows-x64-debug --output-on-failure -R SaveHookIntegrationTests`

Expected: FAIL because no hook adapter is installed.

- [ ] **Step 3: Install adapters all-or-nothing**

Hook the exact profiled paths through `SHGetKnownFolderPath`, `SHGetFolderPathW`, `CreateFileW`, `DeleteFileW`, `MoveFileExW`, `ReplaceFileW`, `GetFileAttributesExW`, and `FindFirstFileExW`. Queue all MinHook changes and enable them together; if any target is absent, roll back the group and return `SAVE_HOOK_INSTALL_FAILED`.

```cpp
const auto decision = policy.Evaluate(PathOperation::Open, canonicalPath);
if (decision.kind == PathDecisionKind::Deny) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
return originalCreateFile(decision.EffectivePath().c_str(), desiredAccess, shareMode, security, creation, flags, templateFile);
```

- [ ] **Step 4: Run CTest repeatedly**

Run: `1..20 | ForEach-Object { ctest --preset windows-x64-debug --output-on-failure -R SaveHookIntegrationTests }`

Expected: 20 PASS runs and writes only below each test's external temporary root.

- [ ] **Step 5: Commit**

```powershell
git add -- native/runtime/save native/fixtures/SaveClientFixture.cpp native/tests/SaveHookIntegrationTests.cpp native/runtime/ProtectionBootstrap.cpp native/include/DSRRandomizer/ProtectionProtocol.h
git commit -m "feat: redirect guarded save io to rmm"
```

### Task 6: Launcher preparation flow with launch still locked

**Files:**
- Modify: `src/DSRRandomizer.Launcher/Services/ILauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs`
- Modify: `src/DSRRandomizer.Launcher/MainWindow.xaml`
- Modify: `src/DSRRandomizer.Launcher/LauncherApplication.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/Services/LauncherServiceTests.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`

**Interfaces:**
- Adds: `DiscoverSaveProfilesAsync` and `PrepareDedicatedSaveAsync` to `ILauncherService`.
- Adds CLI syntax `--prepare-save 12345678901234567` using the exact selected decimal SteamID; success never launches the game.

- [ ] **Step 1: Write UI/CLI tests for zero, one, and multiple profiles**

```csharp
[Fact]
public async Task PrepareSave_ExistingRmmReportsReuseAndKeepsLaunchDisabled()
{
    await ViewModel.PrepareSaveCommand.ExecuteAsync(null);
    Assert.Contains("existing DRAKS0005.rmm", ViewModel.Status, StringComparison.OrdinalIgnoreCase);
    Assert.False(ViewModel.CanLaunch);
}
```

- [ ] **Step 2: Run launcher tests to observe interface failures**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj`

Expected: FAIL until all fake services implement the two new methods.

- [ ] **Step 3: Implement explicit selection and confirmation**

The UI lists only discovered SteamIDs, requires selection when count is greater than one, displays source and external destination before first copy, and invokes `PrepareDedicatedSaveAsync` only after confirmation. CLI requires an exact SteamID argument and returns a stable nonzero error when confirmation cannot be expressed safely; it may reuse an existing valid `.rmm` without confirmation.

```csharp
Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(CancellationToken token);
Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(string steamId, bool firstCopyConfirmed, CancellationToken token);
```

- [ ] **Step 4: Run complete managed/native gates**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Run: `pwsh -File scripts/build-native.ps1 -Configuration Release -Test`

Expected: all tests PASS, `CanLaunch` is false, and `--launch` still exits `2`.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher tests/DSRRandomizer.Launcher.Tests
git commit -m "feat: prepare dedicated rmm from launcher"
```

## Plan 2 Exit Gate

Do not start Plan 3 until existing `.rmm` tests prove zero normal-save opens, bootstrap proves source immutability, native fixture writes only `.rmm`, Overhaul paths are denied, and public launch remains locked.

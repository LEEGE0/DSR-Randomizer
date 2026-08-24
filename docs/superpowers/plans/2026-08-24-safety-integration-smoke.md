# Safety Integration and Smoke Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package the verified guard, prove the integrated external runtime safety boundary, and run one developer-only 30-second copied-game smoke audit while public gameplay remains locked.

**Architecture:** A repository-only smoke tool composes readiness, `.rmm`, supervisor, ETW observation, and before/after immutability snapshots. The distributed launcher receives the native component and compatibility profile but still rejects `--launch` and disables the play button until a future validated seed package exists.

**Tech Stack:** .NET 8/C# 12, C++20/MSVC, CMake/CTest, Windows ETW, PowerShell packaging, GitHub Actions

**Spec:** `docs/superpowers/specs/2026-08-24-native-safety-runtime-design.md`

## Global Constraints

- Real game execution is forbidden until every prior plan exit gate passes in the same commit.
- The only permitted real executable is the manifest-verified copy beneath the external runtime root.
- Smoke duration after resume is at most 30 seconds and the Job Object terminates the process tree.
- Before/after audit proves zero original-game, normal-save, and Overhaul changes.
- The production launcher and copied game never open an Overhaul save; only the separately invoked audit may hash it read-only.
- ETW observation must be available; inability to prove no non-loopback traffic blocks smoke execution.
- `DSRRandomizer.SafetySmoke.exe` and all local captures are prohibited from release archives.
- Public `--launch` remains exit code `2` and WPF `CanLaunch` remains false.

## File Structure

- `tools/DSRRandomizer.SafetySmoke`: repository-only orchestration executable.
- `src/DSRRandomizer.Foundation/Auditing/ImmutableSnapshotService.cs`: deterministic file metadata/hash snapshots.
- `src/DSRRandomizer.Foundation/Auditing/AuditContracts.cs`: scope and result records.
- `src/DSRRandomizer.Launcher/Safety/EtwProcessNetworkObserver.cs`: process-scoped outbound ETW observer.
- `scripts/Test-NativeSafetySmoke.ps1`: guarded local entry point and report writer.
- `tests/DSRRandomizer.Foundation.Tests/Auditing/*Tests.cs`: snapshot/diff tests.
- `tests/DSRRandomizer.Launcher.Tests/Safety/EtwProcessNetworkObserverTests.cs`: synthetic ETW event filtering.
- `tests/DSRRandomizer.Launcher.Tests/Packaging/NativePackageTests.cs`: exact package contents.
- `packaging/package.ps1`: include verified guard/profile/checksums/notices.
- `src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs`: allow only project-built safety files.
- `README.md`, `CHANGELOG.md`: v0.2 safety status and locked-play statement.

---

### Task 1: Deterministic immutability audit

**Files:**
- Create: `src/DSRRandomizer.Foundation/Auditing/AuditContracts.cs`
- Create: `src/DSRRandomizer.Foundation/Auditing/ImmutableSnapshotService.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Auditing/ImmutableSnapshotServiceTests.cs`

**Interfaces:**
- Produces: `CaptureAsync(AuditScope, CancellationToken) -> ImmutableSnapshot`.
- Produces: `Compare(before, after) -> AuditDifference[]` sorted by canonical redacted path.

- [ ] **Step 1: Write add/change/delete/timestamp tests**

```csharp
[Fact]
public async Task Compare_ReportsContentAndTimestampChangesDeterministically()
{
    var before = await Service.CaptureAsync(Scope, default);
    await File.AppendAllTextAsync(Fixture.NormalSave, "mutation");
    var after = await Service.CaptureAsync(Scope, default);
    Assert.Collection(Service.Compare(before, after), d => Assert.Equal(AuditChange.Modified, d.Change));
}
```

Test that exported JSON replaces user/SteamID path segments with stable tokens while the in-memory scope retains canonical paths.

- [ ] **Step 2: Run focused tests and observe failure**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter ImmutableSnapshotServiceTests`

Expected: FAIL because auditing types are absent.

- [ ] **Step 3: Implement bounded read-only snapshots**

Snapshot exact declared files and recursively declared installation roots with path, length, UTC write ticks, attributes, file identity, and SHA-256. Open for `FileAccess.Read` only. The normal save and Overhaul hashes are permitted only when `AuditScope.DiagnosticReadOnly == true`; production services never call this API.

```csharp
public sealed record AuditEntry(string RedactedPath, long Length, long LastWriteUtcTicks, FileAttributes Attributes, string Sha256);
public Task<ImmutableSnapshot> CaptureAsync(AuditScope scope, CancellationToken token);
public IReadOnlyList<AuditDifference> Compare(ImmutableSnapshot before, ImmutableSnapshot after);
```

- [ ] **Step 4: Run focused and full tests**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all tests PASS; a second unchanged snapshot has zero differences.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Auditing tests/DSRRandomizer.Foundation.Tests/Auditing
git commit -m "test: add read-only immutability audit"
```

### Task 2: Process-scoped ETW network observation

**Files:**
- Create: `src/DSRRandomizer.Launcher/Safety/IProcessNetworkObserver.cs`
- Create: `src/DSRRandomizer.Launcher/Safety/EtwProcessNetworkObserver.cs`
- Modify: `src/DSRRandomizer.Launcher/Safety/LaunchContracts.cs`
- Modify: `src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/EtwProcessNetworkObserverTests.cs`

**Interfaces:**
- Produces: `ObserveAsync(int processId, CancellationToken) -> NetworkObservation`.
- Produces: exact event count, local/remote loopback classification, and `ProofAvailable`.
- Produces: `IPreResumeGate.ArmAsync(int processId, CancellationToken)` so ETW proof is established after suspended creation and before injection/resume.

- [ ] **Step 1: Write synthetic ETW event filtering tests**

```csharp
[Fact]
public void Filter_FlagsOnlyTargetProcessNonLoopbackTraffic()
{
    Observer.Accept(Event(pid: 10, remote: "127.0.0.1"));
    Observer.Accept(Event(pid: 10, remote: "8.8.8.8"));
    Observer.Accept(Event(pid: 11, remote: "8.8.4.4"));
    Assert.Single(Observer.Result.NonLoopbackEvents);
}
```

- [ ] **Step 2: Run focused tests**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter EtwProcessNetworkObserverTests`

Expected: FAIL because observer types are absent.

- [ ] **Step 3: Implement ETW lifecycle and fail-closed availability**

Use direct Windows ETW P/Invoke rather than a new NuGet dependency. After the child is created suspended and assigned to its Job Object, start the kernel/network ETW session through `IPreResumeGate`, filter by the exact process ID and descendants reported by the Job Object, classify IPv4/IPv6 loopback, and stop/dispose in `finally`. If provider/session permission or event schema validation fails, return `ProofAvailable = false`; the coordinator terminates the Job without resume.

```csharp
public interface IPreResumeGate
{
    Task<PreResumeGateResult> ArmAsync(int processId, CancellationToken token);
}
```

- [ ] **Step 4: Run synthetic and local fixture tests**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter EtwProcessNetworkObserverTests`

Expected: tests PASS; a harmless fixture's denied external attempt is observed as zero outbound events while guard counters record one denial.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher/Safety/IProcessNetworkObserver.cs src/DSRRandomizer.Launcher/Safety/EtwProcessNetworkObserver.cs src/DSRRandomizer.Launcher/Safety/LaunchContracts.cs src/DSRRandomizer.Launcher/Safety/SafetyLaunchCoordinator.cs tests/DSRRandomizer.Launcher.Tests/Safety/EtwProcessNetworkObserverTests.cs
git commit -m "test: observe guarded process network traffic"
```

### Task 3: Repository-only smoke coordinator

**Files:**
- Create: `tools/DSRRandomizer.SafetySmoke/DSRRandomizer.SafetySmoke.csproj`
- Create: `tools/DSRRandomizer.SafetySmoke/Program.cs`
- Create: `tools/DSRRandomizer.SafetySmoke/SafetySmokeCoordinator.cs`
- Create: `tools/DSRRandomizer.SafetySmoke/SmokeReport.cs`
- Create: `scripts/Test-NativeSafetySmoke.ps1`
- Test: `tests/DSRRandomizer.Launcher.Tests/Safety/SafetySmokeCoordinatorTests.cs`

**Interfaces:**
- Produces: developer command `scripts/Test-NativeSafetySmoke.ps1 -DurationSeconds 30`; the tool resolves the verified runtime pointer and external save selection itself.
- Produces: redacted `safety-smoke-report.json` below external logs.

- [ ] **Step 1: Write precondition and timeout tests with fakes**

```csharp
[Theory]
[InlineData(SmokeFailure.RuntimeNotReady)]
[InlineData(SmokeFailure.SaveNotReady)]
[InlineData(SmokeFailure.NetworkProofUnavailable)]
[InlineData(SmokeFailure.IncompleteProtection)]
public async Task RunAsync_NeverResumesWhenAnyPreconditionFails(SmokeFailure failure)
{
    var harness = SmokeHarness.FailingAt(failure);
    var report = await harness.Coordinator.RunAsync(harness.Request, default);
    Assert.False(report.Resumed);
    Assert.Equal(0, harness.Platform.ResumeCalls);
}
```

- [ ] **Step 2: Run focused tests and observe failure**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter SafetySmokeCoordinatorTests`

Expected: FAIL because the smoke tool/coordinator are absent.

- [ ] **Step 3: Implement the exact orchestration**

Sequence: validate runtime manifest/profile -> validate or bootstrap active `.rmm` -> make and verify a unique diagnostic `.rmm` clone beneath external staging -> configure only the smoke child to use the clone -> capture diagnostic snapshot -> create suspended copied child -> assign Job -> arm ETW proof for the exact PID -> inject/verify full bitmap -> resume once -> record loaded-module and redacted save-operation audits -> monitor for `min(requested, 30 seconds)` -> terminate Job -> wait for exit -> stop ETW -> capture after snapshot -> compare -> write redacted report -> discard only the diagnostic clone. Any exception closes the Job, preserves the active `.rmm`, and writes a failed report outside game paths.

```csharp
await using var child = await _platform.CreateSuspendedAsync(request.Launch, token);
child.AssignKillOnCloseJob();
var proof = await _networkGate.ArmAsync(child.ProcessId, token);
if (!proof.Ready) return await FailWithoutResumeAsync(child, "NETWORK_PROOF_UNAVAILABLE");
return await RunBoundedGuardedSessionAsync(child, TimeSpan.FromSeconds(Math.Min(request.DurationSeconds, 30)), token);
```

- [ ] **Step 4: Run only fake-runtime integration**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter SafetySmokeCoordinatorTests`

Expected: PASS for successful fixture and every failure point; elapsed virtual time never exceeds 30 seconds.

- [ ] **Step 5: Commit**

```powershell
git add -- tools/DSRRandomizer.SafetySmoke scripts/Test-NativeSafetySmoke.ps1 tests/DSRRandomizer.Launcher.Tests/Safety/SafetySmokeCoordinatorTests.cs
git commit -m "test: add bounded external runtime smoke tool"
```

### Task 4: Native component packaging and release guard

**Files:**
- Modify: `src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs`
- Modify: `tests/DSRRandomizer.Foundation.Tests/Packaging/ReleaseContentGuardTests.cs`
- Modify: `packaging/package.ps1`
- Create: `src/DSRRandomizer.Foundation/Runtime/NativeComponentInstaller.cs`
- Test: `tests/DSRRandomizer.Foundation.Tests/Runtime/NativeComponentInstallerTests.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/ILauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs`
- Modify: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Allows exactly: launcher, `DSRRandomizer.Runtime.dll`, `compatibility-profiles.json`, their project manifest/checksum, notices, README, LICENSE, and CHANGELOG.
- Rejects: every PDB, smoke tool, game binary/data, `.sl2`, `.rmm`, capture, local profile, and credential.
- Produces: `NativeComponentInstaller.InstallVerifiedAsync` which atomically installs a hash-verified component set beneath `LocalDataLayout.Components`.
- Adds: `ILauncherService.InstallSafetyComponentsAsync`, called during launcher safety initialization before readiness is reported.

- [ ] **Step 1: Extend allow/deny tests first**

```csharp
[Theory]
[InlineData("DSRRandomizer.SafetySmoke.exe")]
[InlineData("saves/DRAKS0005.rmm")]
[InlineData("compatibility-captures/login.bin")]
public void Validate_RejectsDeveloperAndGameDerivedContent(string path) =>
    Assert.Contains(path, Guard.Validate(new[] { path }));
```

Add `NativeComponentInstallerTests` for a correct manifest, DLL hash mismatch, profile hash mismatch, destination race, and reparse escape. Every failure must preserve the previously active external component directory.

- [ ] **Step 2: Run packaging tests and observe guard rejection**

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter ReleaseContentGuardTests`

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter LauncherApplicationPackageTests`

Run: `dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter NativeComponentInstallerTests`

Expected: the intended native deliverables are rejected until the allowlist and manifest verification are implemented.

- [ ] **Step 3: Package verified native files deterministically**

Copy only the Release guard DLL and release-pinned profile after validating their SHA-256 against a project component manifest. Append the exact MinHook license. Sort ZIP entries and retain the fixed 1980 timestamp behavior. Run the package validator before and after staging.

`NativeComponentInstaller` copies the packaged DLL/profile/manifest to a unique external staging directory, verifies every hash, atomically activates `%LOCALAPPDATA%\DSR-Randomizer\components\0.2.0-alpha.1`, and never writes beside either game executable. `LauncherService.InstallSafetyComponentsAsync` invokes it from the launcher safety-initialization flow; status methods remain read-only.

```csharp
public Task<ComponentInstallResult> InstallVerifiedAsync(
    string packageComponentRoot, string version, CancellationToken token);
```

- [ ] **Step 4: Build and inspect a release package**

Run: `dotnet publish src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release -r win-x64 --self-contained true -o artifacts/publish`

Run: `pwsh -File packaging/package.ps1 -Version 0.2.0-alpha.1 -PublishPath artifacts/publish -NativePath native/out/build/windows-x64-release -OutputPath artifacts`

Expected: validator exit `0`; archive contains only the exact allowlist and no smoke executable, game file, save, or capture.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs src/DSRRandomizer.Foundation/Runtime/NativeComponentInstaller.cs src/DSRRandomizer.Launcher/Services/ILauncherService.cs src/DSRRandomizer.Launcher/Services/LauncherService.cs tests/DSRRandomizer.Foundation.Tests/Packaging/ReleaseContentGuardTests.cs tests/DSRRandomizer.Foundation.Tests/Runtime/NativeComponentInstallerTests.cs packaging/package.ps1 tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs THIRD_PARTY_NOTICES.md
git commit -m "build: package verified native safety guard"
```

### Task 5: Product status without gameplay unlock

**Files:**
- Modify: `src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs`
- Modify: `src/DSRRandomizer.Launcher/MainWindow.xaml`
- Modify: `src/DSRRandomizer.Launcher/LauncherApplication.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/ILauncherService.cs`
- Modify: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Create: `src/DSRRandomizer.Foundation/Safety/SafetyReadinessResult.cs`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Test: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`
- Test: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`

**Interfaces:**
- Preserves: `CanLaunch == false` and `--launch` exit `2`.
- Reports independently: external runtime ready, `.rmm` ready, safety component verified, active seed missing.
- Adds: `ILauncherService.GetSafetyReadinessAsync` and `GetDedicatedSaveReadinessAsync` for read-only status refresh.

- [ ] **Step 1: Write the seed-missing lock test**

```csharp
[Fact]
public async Task CompleteSafetyWithoutActiveSeed_StillCannotLaunch()
{
    Service.SafetyReadiness = new SafetyReadinessResult(true, Array.Empty<string>());
    Service.SaveReadiness = new DedicatedSaveResult(
        true, true, @"C:\Local\saves\12345678901234567\DRAKS0005.rmm",
        SaveErrorCode.None, "existing dedicated save is ready");
    await ViewModel.RefreshAsync();
    Assert.False(ViewModel.CanLaunch);
    Assert.Contains("validated seed", ViewModel.Status, StringComparison.OrdinalIgnoreCase);
}
```

- [ ] **Step 2: Run launcher tests and observe old status failure**

Run: `dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter "MainWindowViewModelTests|LauncherApplicationTests"`

Expected: FAIL until the view model distinguishes safety readiness from seed readiness.

- [ ] **Step 3: Implement truthful v0.2 status and documentation**

Do not add a launch command. Change the banner to state that native safety and `.rmm` preparation are available but play remains locked until deterministic seed generation/validation exists. Document that the diagnostic smoke tool is repository-only and never shipped.

On launcher startup, call `InstallSafetyComponentsAsync` once, then use only read-only readiness methods for subsequent refreshes. Installation failure reports the stable safety error and leaves the previous verified component version active.

```csharp
public bool CanLaunch => false;

public async Task RefreshAsync()
{
    await _service.InstallSafetyComponentsAsync(CancellationToken.None);
    SafetyReadiness = await _service.GetSafetyReadinessAsync(CancellationToken.None);
}
```

- [ ] **Step 4: Run managed tests**

Run: `dotnet test DSR-Randomizer.sln -c Release --no-restore`

Expected: all tests PASS; `Program.Main(["--launch"])` returns `2`.

- [ ] **Step 5: Commit**

```powershell
git add -- src/DSRRandomizer.Launcher README.md CHANGELOG.md tests/DSRRandomizer.Launcher.Tests
git commit -m "docs: report native safety alpha status"
```

### Task 6: Controlled copied-game smoke audit

**Files:**
- Generated only outside repository: `%LOCALAPPDATA%\DSR-Randomizer\logs\safety-smoke-report.json`
- No source file changes in this task unless a test exposes a defect; any defect returns to TDD in its owning task before retry.

**Interfaces:**
- Consumes: verified external runtime, selected SteamID, valid `.rmm`, complete guard/profile, ETW proof.
- Produces: one redacted pass/fail audit report and terminated copied-game process tree.

- [ ] **Step 1: Record the exact pre-smoke commit and run every automated gate**

Run: `git rev-parse HEAD`

Run: `pwsh -File scripts/build-native.ps1 -Configuration Release -Test`

Run: `dotnet build DSR-Randomizer.sln -c Release --no-restore`

Run: `dotnet test DSR-Randomizer.sln -c Release --no-build`

Run: `dotnet run --project tools/DSRRandomizer.ProfileInspector -- verify "C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe" config/compatibility-profiles.json`

Expected: every command exits `0`; any failure forbids the smoke command.

- [ ] **Step 2: Verify product lock and external-runtime path**

Run: `dotnet run --project src/DSRRandomizer.Launcher -- --launch`

Expected: exit `2`.

Run: `dotnet run --project src/DSRRandomizer.Launcher -- --status`

Expected: ready runtime path is below `%LOCALAPPDATA%\DSR-Randomizer\runtimes`, never below the Steam installation.

- [ ] **Step 3: Execute the bounded diagnostic once**

Run: `pwsh -File scripts/Test-NativeSafetySmoke.ps1 -DurationSeconds 30`

Expected: the tool resolves the external runtime pointer and selected SteamID from external configuration, creates a verified diagnostic `.rmm` clone, resumes the copied process only after full protection, runs no longer than 30 seconds, terminates it through its Job Object, and leaves the active `.rmm` unchanged.

- [ ] **Step 4: Validate the generated report**

Expected report fields:

```json
{
  "durationSeconds": 30,
  "fullProtectionBeforeResume": true,
  "nonLoopbackOutboundEvents": 0,
  "originalInstallationDifferences": 0,
  "normalSaveDifferences": 0,
  "overhaulDifferences": 0,
  "activeRmmDifferences": 0,
  "overhaulProductionReads": 0,
  "installedOverhaulModulesLoaded": 0,
  "dedicatedRmmWasOnlyWritableSave": true,
  "processTreeExited": true
}
```

Any false/nonzero value is a release blocker. Preserve the external report; do not copy it into Git.

- [ ] **Step 5: Commit only a verified release-readiness record**

Add a redacted one-line result with commit SHA and no local paths/SteamID to `CHANGELOG.md`, then:

```powershell
git add -- CHANGELOG.md
git commit -m "test: record native safety smoke verification"
```

### Task 7: CI and branch review gate

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- CI builds/tests/packages Plans 1-4 artifacts but never runs the owned-game smoke tool.

- [ ] **Step 1: Add CI assertions for native/package artifacts**

Build Release native first, run CTest, build/test managed code, publish launcher, remove/reject every PDB, package `0.2.0-alpha.1`, validate the archive, and upload only ZIP/checksum.

```yaml
- run: cmake --preset windows-x64-release
- run: cmake --build --preset windows-x64-release
- run: ctest --preset windows-x64-release --output-on-failure
- run: dotnet test DSR-Randomizer.sln -c Release --no-build
```

- [ ] **Step 2: Run the local CI-equivalent command sequence**

Run: `cmake --preset windows-x64-release`

Run: `cmake --build --preset windows-x64-release`

Run: `ctest --preset windows-x64-release --output-on-failure`

Run: `dotnet build DSR-Randomizer.sln -c Release --no-restore`

Run: `dotnet test DSR-Randomizer.sln -c Release --no-build`

Run: `dotnet publish src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release -r win-x64 --self-contained true -o artifacts/publish`

Run: `pwsh -File packaging/package.ps1 -Version 0.2.0-alpha.1 -PublishPath artifacts/publish -NativePath native/out/build/windows-x64-release -OutputPath artifacts`

Expected: zero warnings/errors/failures and deterministic package entries.

- [ ] **Step 3: Verify repository hygiene**

Run: `git diff --check`

Run: `git status --short`

Run: `rg -n "DRAKS0005\.sl2|DRAKS0005\.rmm|compatibility-captures|SteamId" artifacts -g '*'`

Expected: the archive contains none of the prohibited local/game-derived content; only documentation/tests may contain literal generic filenames.

- [ ] **Step 4: Commit CI integration**

```powershell
git add -- .github/workflows/ci.yml
git commit -m "ci: verify native safety release package"
```

- [ ] **Step 5: Request code review before merge/tag**

Create a draft PR for the implementation branch. Do not merge, tag `v0.2.0-alpha.1`, or publish a release until review, CI, and the controlled local smoke audit all pass and the user explicitly approves release publication.

## Plan 4 Exit Gate

This milestone is ready for release review only when the controlled report has all required zero/true values, package inspection finds no game/save/capture content, CI passes, and the public launcher still cannot start gameplay without a validated seed.

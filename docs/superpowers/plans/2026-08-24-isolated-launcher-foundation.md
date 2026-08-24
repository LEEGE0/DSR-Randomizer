# Isolated Launcher Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `v0.1.0-alpha.1`, a Windows x64 foundation launcher that verifies a Dark Souls Remastered installation read-only, creates and validates a full external runtime copy, proves the source installation was unchanged, and refuses to start the game until dedicated-save and official-online-blocking safety components exist.

**Architecture:** A WPF launcher calls a testable foundation library. The foundation library owns canonical path enforcement, stock-file selection, hashing, staged full-copy generation, a small atomic current-runtime pointer, and runtime-readiness validation. Game-derived data lives only under `%LOCALAPPDATA%\DSR-Randomizer`; the repository and release archive contain no game files. This phase contains no `Process.Start` path for the game.

**Tech Stack:** C# 12, .NET 8 SDK, WPF, xUnit, Windows x64, SHA-256, GitHub Actions

**Spec:** `docs/superpowers/specs/2026-08-24-dsr-randomizer-design.md`

## Global Constraints

- Target Windows x64 and `net8.0-windows`; publish the launcher self-contained.
- Treat the selected Steam installation and every descendant as read-only.
- Permit writes only beneath `%LOCALAPPDATA%\DSR-Randomizer` after final-path canonicalization.
- Create a full file copy; never create hard links, symbolic links, or junctions for runtime game files.
- Never copy installed `d3d11.dll`, `d3d11_mod.ini`, `overhaul`, `DSRQuickSummonCompanion.dll`, mod logs, or other unlisted root files.
- Never commit or package game executables, game DLLs, `.dcx`, `.bnd`, `.bhd`, `.bdt`, saves, local catalogs, credentials, or generated runtime data.
- Keep the repository public under `GPL-3.0-only` and preserve required third-party notices.
- Use TDD for every behavior task; run the named focused test before the full suite.
- Use Conventional Commits and push each completed task branch state.
- Do not implement item randomization, enemy randomization, native runtime hooks, save redirection, auto-equip, or online blocking in this plan.
- Because save redirection and online blocking are absent, this release must never start `DarkSoulsRemastered.exe`; the launch control remains visibly locked.

---

## Planned File Structure

```text
DSR-Randomizer.sln
global.json
Directory.Build.props
src/
├── DSRRandomizer.Foundation/
│   ├── DSRRandomizer.Foundation.csproj
│   ├── Paths/
│   │   ├── IPathCanonicalizer.cs
│   │   ├── WindowsPathCanonicalizer.cs
│   │   ├── WriteBoundary.cs
│   │   └── LocalDataLayout.cs
│   ├── Installation/
│   │   ├── StockGameLayout.cs
│   │   ├── GameInstallationVerifier.cs
│   │   ├── GameFileCatalog.cs
│   │   └── VerificationResult.cs
│   ├── Runtime/
│   │   ├── FileHashService.cs
│   │   ├── IFileCopier.cs
│   │   ├── SystemFileCopier.cs
│   │   ├── IDiskSpaceProbe.cs
│   │   ├── DriveDiskSpaceProbe.cs
│   │   ├── IClock.cs
│   │   ├── SystemClock.cs
│   │   ├── SourceSnapshot.cs
│   │   ├── RuntimeManifest.cs
│   │   ├── RuntimeBuilder.cs
│   │   ├── RuntimePointerStore.cs
│   │   └── RuntimeReadinessService.cs
│   └── Packaging/
│       └── ReleaseContentGuard.cs
└── DSRRandomizer.Launcher/
    ├── DSRRandomizer.Launcher.csproj
    ├── App.xaml
    ├── App.xaml.cs
    ├── MainWindow.xaml
    ├── MainWindow.xaml.cs
    ├── LauncherApplication.cs
    ├── Services/
    │   ├── ILauncherService.cs
    │   └── LauncherService.cs
    ├── Logging/
    │   ├── IExternalLogger.cs
    │   └── FileExternalLogger.cs
    └── ViewModels/
        ├── ObservableObject.cs
        ├── AsyncRelayCommand.cs
        └── MainWindowViewModel.cs
tests/
├── DSRRandomizer.Foundation.Tests/
│   ├── DSRRandomizer.Foundation.Tests.csproj
│   ├── Paths/WriteBoundaryTests.cs
│   ├── Installation/GameInstallationVerifierTests.cs
│   ├── Runtime/RuntimeBuilderTests.cs
│   ├── Runtime/RuntimeReadinessServiceTests.cs
│   └── Packaging/ReleaseContentGuardTests.cs
└── DSRRandomizer.Launcher.Tests/
    ├── DSRRandomizer.Launcher.Tests.csproj
    ├── LauncherApplicationTests.cs
    ├── LauncherApplicationPackageTests.cs
    └── ViewModels/MainWindowViewModelTests.cs
packaging/package.ps1
scripts/Test-OriginalInstallUnchanged.ps1
.github/workflows/ci.yml
CHANGELOG.md
THIRD_PARTY_NOTICES.md
```

`DSRRandomizer.Foundation` contains no WPF references. `DSRRandomizer.Launcher` contains UI and orchestration only. All filesystem and process behavior is behind interfaces so unit tests never need the real game.

---

### Task 1: Solution Skeleton and Enforced Write Boundary

**Files:**
- Create: `global.json`
- Create: `Directory.Build.props`
- Create: `DSR-Randomizer.sln`
- Create: `src/DSRRandomizer.Foundation/DSRRandomizer.Foundation.csproj`
- Create: `src/DSRRandomizer.Foundation/Paths/IPathCanonicalizer.cs`
- Create: `src/DSRRandomizer.Foundation/Paths/WindowsPathCanonicalizer.cs`
- Create: `src/DSRRandomizer.Foundation/Paths/WriteBoundary.cs`
- Create: `src/DSRRandomizer.Foundation/Paths/LocalDataLayout.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj`
- Create: `tests/DSRRandomizer.Foundation.Tests/Paths/WriteBoundaryTests.cs`

**Interfaces:**
- Produces: `IPathCanonicalizer.Canonicalize(string path) : string`
- Produces: `WriteBoundary.Create(string sourceInstallation, string localDataRoot, IPathCanonicalizer canonicalizer) : WriteBoundary`
- Produces: `WriteBoundary.EnsureAllowed(string candidatePath) : void`
- Produces: `LocalDataLayout.Create(string localDataRoot, WriteBoundary boundary) : LocalDataLayout`
- Produces: `LocalDataLayout` properties `Root`, `Runtimes`, `Staging`, `ActiveSeed`, `Saves`, `Config`, and `Logs`

- [ ] **Step 1: Install and pin the .NET 8 SDK**

Run:

```powershell
winget install --id Microsoft.DotNet.SDK.8 --exact --accept-package-agreements --accept-source-agreements
dotnet --list-sdks
```

Expected: output includes one `8.0.x` SDK. Create `global.json` with `rollForward` set to `latestPatch`, and set `TargetFramework`, nullable reference types, implicit usings, deterministic builds, and warnings-as-errors in `Directory.Build.props`.

- [ ] **Step 2: Scaffold the solution and test project**

Run:

```powershell
dotnet new sln -n DSR-Randomizer
dotnet new classlib -n DSRRandomizer.Foundation -o src/DSRRandomizer.Foundation --framework net8.0
dotnet new xunit -n DSRRandomizer.Foundation.Tests -o tests/DSRRandomizer.Foundation.Tests --framework net8.0
dotnet sln DSR-Randomizer.sln add src/DSRRandomizer.Foundation/DSRRandomizer.Foundation.csproj
dotnet sln DSR-Randomizer.sln add tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj
dotnet add tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj reference src/DSRRandomizer.Foundation/DSRRandomizer.Foundation.csproj
```

Delete only the generated `Class1.cs` and `UnitTest1.cs` with `apply_patch`.

- [ ] **Step 3: Write failing path-boundary tests**

Add tests that map a textual alias to the protected source path and prove both direct and alias writes are rejected:

```csharp
[Fact]
public void EnsureAllowed_RejectsSourceDescendantAndResolvedAlias()
{
    var canonicalizer = new FakeCanonicalizer(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
    {
        [@"C:\Steam\DSR"] = @"C:\Steam\DSR",
        [@"C:\Steam\DSR\param\file.dcx"] = @"C:\Steam\DSR\param\file.dcx",
        [@"C:\Local\DSR"] = @"C:\Local\DSR",
        [@"C:\Local\DSR\escape\file.dcx"] = @"C:\Steam\DSR\param\file.dcx"
    });
    var boundary = WriteBoundary.Create(@"C:\Steam\DSR", @"C:\Local\DSR", canonicalizer);

    Assert.Throws<UnauthorizedAccessException>(() => boundary.EnsureAllowed(@"C:\Steam\DSR\param\file.dcx"));
    Assert.Throws<UnauthorizedAccessException>(() => boundary.EnsureAllowed(@"C:\Local\DSR\escape\file.dcx"));
}

[Fact]
public void EnsureAllowed_AllowsOnlyLocalRootDescendants()
{
    var canonicalizer = new IdentityCanonicalizer();
    var boundary = WriteBoundary.Create(@"C:\Steam\DSR", @"C:\Local\DSR", canonicalizer);

    boundary.EnsureAllowed(@"C:\Local\DSR\staging\runtime.json");
    Assert.Throws<UnauthorizedAccessException>(() => boundary.EnsureAllowed(@"C:\Other\runtime.json"));
}
```

Add tests that `WriteBoundary.Create` rejects equal or overlapping canonical source/local roots in either direction. Add canonicalizer tests for drive paths, `\\?\` drive paths, and `\\?\UNC\server\share` conversion to `\\server\share`; resolution failure must throw before any write.

- [ ] **Step 4: Run the tests to verify they fail**

Run:

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~WriteBoundaryTests
```

Expected: FAIL because `WriteBoundary`, `IPathCanonicalizer`, and the test fakes do not exist.

- [ ] **Step 5: Implement canonical paths, the boundary, and local layout**

Implement the boundary with an explicit segment-safe descendant check:

```csharp
public sealed class WriteBoundary
{
    private readonly string _sourceRoot;
    private readonly string _localRoot;
    private readonly IPathCanonicalizer _canonicalizer;

    private WriteBoundary(string sourceRoot, string localRoot, IPathCanonicalizer canonicalizer)
        => (_sourceRoot, _localRoot, _canonicalizer) = (sourceRoot, localRoot, canonicalizer);

    public static WriteBoundary Create(string sourceInstallation, string localDataRoot, IPathCanonicalizer canonicalizer)
        => new(canonicalizer.Canonicalize(sourceInstallation), canonicalizer.Canonicalize(localDataRoot), canonicalizer);

    public void EnsureAllowed(string candidatePath)
    {
        var candidate = _canonicalizer.Canonicalize(candidatePath);
        if (IsAtOrBelow(candidate, _sourceRoot) || !IsAtOrBelow(candidate, _localRoot))
            throw new UnauthorizedAccessException($"Write denied outside the randomizer data root: {candidate}");
    }

    private static bool IsAtOrBelow(string candidate, string root)
        => candidate.Equals(root, StringComparison.OrdinalIgnoreCase)
           || candidate.StartsWith(root.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
}
```

`WindowsPathCanonicalizer` must resolve the nearest existing ancestor through a Windows file handle and `GetFinalPathNameByHandleW`, then append nonexistent path segments. Strip the `\\?\` prefix before comparison. Fail closed when resolution fails.
Normalize `\\?\C:\...` to `C:\...` and `\\?\UNC\server\share\...` to `\\server\share\...`. `WriteBoundary.Create` must reject source/local roots when either is equal to or a descendant of the other.

- [ ] **Step 6: Run focused and full tests**

Run:

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~WriteBoundaryTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS with zero warnings.

- [ ] **Step 7: Commit and push**

```powershell
git add -- global.json Directory.Build.props DSR-Randomizer.sln src/DSRRandomizer.Foundation tests/DSRRandomizer.Foundation.Tests
git commit -m "feat: enforce isolated write boundaries"
git push
```

---

### Task 2: Read-Only Installation Verification and Copy Catalog

**Files:**
- Create: `src/DSRRandomizer.Foundation/Installation/StockGameLayout.cs`
- Create: `src/DSRRandomizer.Foundation/Installation/GameInstallationVerifier.cs`
- Create: `src/DSRRandomizer.Foundation/Installation/GameFileCatalog.cs`
- Create: `src/DSRRandomizer.Foundation/Installation/VerificationResult.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Installation/GameInstallationVerifierTests.cs`

**Interfaces:**
- Consumes: `IPathCanonicalizer.Canonicalize(string)`
- Produces: `GameInstallationVerifier.VerifyAsync(string installationPath, CancellationToken cancellationToken) : Task<VerificationResult>`
- Produces: `VerificationResult(bool IsValid, string CanonicalInstallationPath, GameFileCatalog? Catalog, IReadOnlyList<string> Errors)`
- Produces: `GameFileCatalog(IReadOnlyList<GameFileEntry> Files, long TotalBytes)`
- Produces: `GameFileEntry(string RelativePath, long Length, DateTime LastWriteTimeUtc)`

- [ ] **Step 1: Write failing verifier tests**

Create a temporary fake installation and assert that only explicitly allowed content enters the catalog:

```csharp
[Fact]
public async Task VerifyAsync_ExcludesInstalledModsAndRequiresGameExecutable()
{
    using var tree = FakeGameTree.Create();
    tree.Write("DarkSoulsRemastered.exe", "game");
    tree.Write("steam_api64.dll", "steam");
    tree.Write("map/MapStudio/m10_00_00_00.msb.dcx", "map");
    tree.Write("d3d11.dll", "overhaul loader");
    tree.Write("d3d11_mod.ini", "overhaul config");
    tree.Write("overhaul/GameParam.parambnd.dcx", "overhaul data");
    tree.Write("DSRQuickSummonCompanion.dll", "companion");

    var result = await CreateVerifier().VerifyAsync(tree.Root, CancellationToken.None);

    Assert.True(result.IsValid);
    Assert.Equal(
        new[] { "DarkSoulsRemastered.exe", "map/MapStudio/m10_00_00_00.msb.dcx", "steam_api64.dll" },
        result.Catalog!.Files.Select(file => file.RelativePath).Order());
}
```

Add separate tests for a missing executable, a missing required data directory, a nonexistent path, and a path that resolves inside the randomizer local-data root.

- [ ] **Step 2: Run the focused test to verify failure**

Run:

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~GameInstallationVerifierTests
```

Expected: FAIL because the installation types do not exist.

- [ ] **Step 3: Implement an explicit stock layout**

`StockGameLayout` must enumerate only these root files:

```csharp
public static readonly ImmutableHashSet<string> RootFiles =
    ImmutableHashSet.Create(StringComparer.OrdinalIgnoreCase,
        "DarkSoulsRemastered.exe",
        "steam_api64.dll",
        "binkw64.dll",
        "fmod_event_net64.dll",
        "fmod_event64.dll",
        "fmodex64.dll",
        "xinput1_3.dll");

public static readonly ImmutableHashSet<string> DataDirectories =
    ImmutableHashSet.Create(StringComparer.OrdinalIgnoreCase,
        "chr", "event", "facegen", "font", "map", "menu", "movww", "msg",
        "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
        "sfx", "shader", "sound");
```

Enumerate each allowed directory recursively. Reject every directory or file carrying `FileAttributes.ReparsePoint`; do not traverse it. Normalize catalog separators to `/`, sort with `StringComparer.Ordinal`, reject duplicate normalized paths, and perform no writes. Root entries not present in the two allowlists—including the installed Overhaul loader/configuration, companion DLL, `crash`, logs, credentials, `VERSION`, and `searchlist.txt`—never enter the catalog.

- [ ] **Step 4: Run focused and full tests**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~GameInstallationVerifierTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS.

- [ ] **Step 5: Commit and push**

```powershell
git add -- src/DSRRandomizer.Foundation/Installation tests/DSRRandomizer.Foundation.Tests/Installation
git commit -m "feat: verify source installation read only"
git push
```

---

### Task 3: Staged Full-Copy Runtime Builder

**Files:**
- Create: `src/DSRRandomizer.Foundation/Runtime/FileHashService.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/IFileCopier.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/SystemFileCopier.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/IDiskSpaceProbe.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/DriveDiskSpaceProbe.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/IClock.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/SystemClock.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/SourceSnapshot.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/RuntimeManifest.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/RuntimeBuilder.cs`
- Create: `src/DSRRandomizer.Foundation/Runtime/RuntimePointerStore.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Runtime/RuntimeBuilderTests.cs`

**Interfaces:**
- Consumes: `GameFileCatalog`, `LocalDataLayout`, and `WriteBoundary`
- Consumes: constructor dependencies `IFileCopier`, `IDiskSpaceProbe`, `IClock`, `FileHashService`, and `RuntimePointerStore`
- Produces: `IFileCopier.Copy(string source, string destination) : void`
- Produces: `IDiskSpaceProbe.GetAvailableBytes(string path) : long`
- Produces: `IClock.UtcNow : DateTimeOffset`
- Produces: `FileHashService.ComputeSha256Async(string path, CancellationToken cancellationToken) : Task<string>`
- Produces: `SourceSnapshot.CaptureAsync(string sourceRoot, GameFileCatalog catalog, FileHashService hashes, CancellationToken cancellationToken) : Task<SourceSnapshot>`
- Produces: `RuntimeBuildProgress(long CopiedBytes, long TotalBytes, string RelativePath)`
- Produces: `RuntimeBuilder.BuildAsync(string sourceRoot, GameFileCatalog catalog, IProgress<RuntimeBuildProgress>? progress, CancellationToken cancellationToken) : Task<RuntimeManifest>`
- Produces: `RuntimePointer(string RuntimeId, string RelativeRuntimePath, string ManifestSha256)`
- Produces: `RuntimePointerStore.ReadAsync(CancellationToken cancellationToken) : Task<RuntimePointer?>`
- Produces: `RuntimePointerStore.ActivateAsync(RuntimePointer pointer, CancellationToken cancellationToken) : Task`

- [ ] **Step 1: Write failing full-copy and rollback tests**

```csharp
[Fact]
public async Task BuildAsync_CopiesBytesWithoutHardLinksAndLeavesSourceUnchanged()
{
    using var source = FakeGameTree.WithRequiredFiles();
    using var local = TemporaryDirectory.Create();
    var before = await TestSnapshot.CaptureAsync(source.Root);

    var manifest = await CreateBuilder(local.Root, source.Root).BuildAsync(
        source.Root, CreateCatalog(source.Root), progress: null, CancellationToken.None);

    var copiedExe = Path.Combine(manifest.RuntimePath, "DarkSoulsRemastered.exe");
    Assert.Equal(File.ReadAllBytes(source.PathOf("DarkSoulsRemastered.exe")), File.ReadAllBytes(copiedExe));
    Assert.NotEqual(GetFileIdentity(source.PathOf("DarkSoulsRemastered.exe")), GetFileIdentity(copiedExe));
    Assert.Equal(before, await TestSnapshot.CaptureAsync(source.Root));
}

[Fact]
public async Task BuildAsync_CopyFailurePreservesCurrentPointerAndExistingRuntime()
{
    var fixture = RuntimeBuilderFixture.WithActiveRuntime("runtime-old");
    fixture.FileCopier.FailOn("map/MapStudio/m10_00_00_00.msb.dcx");

    await Assert.ThrowsAsync<IOException>(() => fixture.Builder.BuildAsync(
        fixture.SourceRoot, fixture.Catalog, null, CancellationToken.None));

    Assert.Equal("runtime-old", (await fixture.PointerStore.ReadAsync(CancellationToken.None))!.RuntimeId);
    Assert.True(Directory.Exists(fixture.OldRuntimePath));
}
```

Also test insufficient disk space, source mutation during copy, destination hash mismatch, and denied staging paths.

- [ ] **Step 2: Run focused tests to verify failure**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~RuntimeBuilderTests
```

Expected: FAIL because runtime-building types do not exist.

- [ ] **Step 3: Implement immutable generations and atomic pointer activation**

Use this generation layout:

```text
%LOCALAPPDATA%\DSR-Randomizer\runtimes\runtime-<content-descriptor-hash>\
%LOCALAPPDATA%\DSR-Randomizer\runtime-current.json
```

Build into `staging\runtime-<guid>`. Before copy, require `catalog.TotalBytes + 536870912` available bytes. Capture a complete source snapshot containing path, length, last-write time, and SHA-256. For every file: call `WriteBoundary.EnsureAllowed(destination)`, create the parent under staging, call `File.Copy(source, destination, overwrite: false)`, and require the destination SHA-256 to equal the pre-copy source SHA-256. Capture the complete source snapshot again after all copies and require exact equality with the first snapshot. Move the verified staging directory to its immutable runtime ID, then atomically replace only `runtime-current.json` using a same-directory temporary file and `File.Move(temp, current, overwrite: true)`.

Compute the runtime ID as `runtime-` plus the SHA-256 of a deterministic content descriptor containing schema version, source executable SHA-256, catalog SHA-256, total bytes, and every sorted relative path/hash pair. The descriptor excludes runtime ID and creation timestamp, preventing a self-referential hash. `RuntimeManifest` must contain schema version `1`, the resulting runtime ID, creation timestamp, and the descriptor fields. JSON serialization must use camelCase and deterministic path ordering. `runtime-current.json` contains the runtime ID, its relative runtime directory, and the SHA-256 of the serialized manifest.

- [ ] **Step 4: Run focused and full tests**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~RuntimeBuilderTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS and the source-snapshot equality assertion passes.

- [ ] **Step 5: Commit and push**

```powershell
git add -- src/DSRRandomizer.Foundation/Runtime tests/DSRRandomizer.Foundation.Tests/Runtime/RuntimeBuilderTests.cs
git commit -m "feat: build immutable external runtimes"
git push
```

---

### Task 4: Runtime Readiness Validation and Non-Launching CLI

**Files:**
- Create: `src/DSRRandomizer.Foundation/Runtime/RuntimeReadinessService.cs`
- Create: `src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj`
- Create: `src/DSRRandomizer.Launcher/LauncherApplication.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Runtime/RuntimeReadinessServiceTests.cs`
- Create: `tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj`
- Create: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationTests.cs`
- Modify: `DSR-Randomizer.sln`

**Interfaces:**
- Consumes: `RuntimePointerStore.ReadAsync`, `RuntimeManifest`, and `WriteBoundary`
- Produces: `RuntimeReadinessResult(bool IsReady, string? RuntimePath, IReadOnlyList<string> Errors)`
- Produces: `RuntimeReadinessService.ValidateAsync(CancellationToken cancellationToken) : Task<RuntimeReadinessResult>`
- Produces: `LauncherApplication.RunAsync(string[] args, CancellationToken cancellationToken) : Task<int>`

- [ ] **Step 1: Write failing readiness and launch-denial tests**

```csharp
[Fact]
public async Task ValidateAsync_AcceptsOnlyCopiedExecutableOutsideInstalledRoot()
{
    var fixture = RuntimeReadinessFixture.Create(
        installedRoot: @"C:\Steam\DSR",
        runtimeRoot: @"C:\Local\DSR\runtimes\runtime-abc");

    var result = await fixture.Service.ValidateAsync(CancellationToken.None);

    Assert.True(result.IsReady);
    Assert.Equal(@"C:\Local\DSR\runtimes\runtime-abc", result.RuntimePath);
    Assert.DoesNotContain(@"C:\Steam\DSR", result.RuntimePath!, StringComparison.OrdinalIgnoreCase);
}

[Fact]
public async Task RunAsync_LaunchArgumentIsRejectedWithoutStartingAProcess()
{
    var application = CreateApplication();

    var exitCode = await application.RunAsync(new[] { "--launch" }, CancellationToken.None);

    Assert.Equal(2, exitCode);
}
```

Put the first test and the remaining readiness cases in `RuntimeReadinessServiceTests.cs`; put the CLI rejection test in `LauncherApplicationTests.cs`. Add a launcher assembly scan test proving no launcher method calls `System.Diagnostics.Process.Start`. Add readiness tests that reject a missing pointer, a manifest hash mismatch, a copied-file hash mismatch, a missing copied executable, and a runtime path that resolves into the source installation.

- [ ] **Step 2: Run focused tests to verify failure**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~RuntimeReadinessServiceTests
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~LauncherApplicationTests
```

Expected: both commands FAIL because readiness and launcher application types do not exist.

- [ ] **Step 3: Implement readiness validation and CLI commands**

`RuntimeReadinessService` canonicalizes the pointed runtime, proves it is below the local runtime root and outside the source installation, verifies the manifest hash and every manifest file hash, and returns readiness without writing or starting a process.

Configure `DSRRandomizer.Launcher.csproj` with `RuntimeIdentifier` `win-x64`, `SelfContained` `true`, `PublishSingleFile` `true`, and `IncludeNativeLibrariesForSelfExtract` `true`. A release publish therefore emits the project launcher as one executable plus its optional PDB; framework and project DLLs are not loose package entries.

`LauncherApplication` supports exactly these foundation commands:

```text
--verify <game-path>
--initialize-runtime <game-path>
--status
```

Each command writes a single JSON result to stdout and sends diagnostics to stderr. Exit codes are `0` success, `2` invalid or unsupported arguments, `3` verification failure, `4` runtime-build failure, and `5` readiness failure. `--launch` is unsupported in this release and returns `2`. Neither the launcher project nor its dependencies may reference `Process.Start`.

- [ ] **Step 4: Run focused and full tests**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~RuntimeReadinessServiceTests
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~LauncherApplicationTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS.

- [ ] **Step 5: Commit and push**

```powershell
git add -- DSR-Randomizer.sln src/DSRRandomizer.Foundation/Runtime/RuntimeReadinessService.cs src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj src/DSRRandomizer.Launcher/LauncherApplication.cs tests/DSRRandomizer.Foundation.Tests/Runtime/RuntimeReadinessServiceTests.cs tests/DSRRandomizer.Launcher.Tests
git commit -m "feat: validate external runtime readiness"
git push
```

---

### Task 5: WPF Launcher UI

**Files:**
- Create: `src/DSRRandomizer.Launcher/App.xaml`
- Create: `src/DSRRandomizer.Launcher/App.xaml.cs`
- Create: `src/DSRRandomizer.Launcher/MainWindow.xaml`
- Create: `src/DSRRandomizer.Launcher/MainWindow.xaml.cs`
- Create: `src/DSRRandomizer.Launcher/Services/ILauncherService.cs`
- Create: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Create: `src/DSRRandomizer.Launcher/Logging/IExternalLogger.cs`
- Create: `src/DSRRandomizer.Launcher/Logging/FileExternalLogger.cs`
- Create: `src/DSRRandomizer.Launcher/ViewModels/ObservableObject.cs`
- Create: `src/DSRRandomizer.Launcher/ViewModels/AsyncRelayCommand.cs`
- Create: `src/DSRRandomizer.Launcher/ViewModels/MainWindowViewModel.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj`
- Create: `tests/DSRRandomizer.Launcher.Tests/ViewModels/MainWindowViewModelTests.cs`
- Modify: `DSR-Randomizer.sln`

**Interfaces:**
- Consumes: `LauncherApplication` operations through `ILauncherService`
- Produces: `ILauncherService.VerifyAsync(string gamePath, CancellationToken cancellationToken) : Task<VerificationResult>`
- Produces: `ILauncherService.InitializeRuntimeAsync(string gamePath, IProgress<RuntimeBuildProgress>? progress, CancellationToken cancellationToken) : Task<RuntimeManifest>`
- Produces: `ILauncherService.GetReadinessAsync(CancellationToken cancellationToken) : Task<RuntimeReadinessResult>`
- Produces: `IExternalLogger.LogExceptionAsync(Exception exception, CancellationToken cancellationToken) : Task`
- Produces: `MainWindowViewModel.GamePath`, `Status`, `ProgressPercent`, `IsBusy`, `CanLaunch`, `VerifyCommand`, and `InitializeCommand`

- [ ] **Step 1: Write failing view-model state tests**

```csharp
[Fact]
public async Task InitializeCommand_ReportsReadyButKeepsLaunchSafetyLocked()
{
    var service = new FakeLauncherService { VerifyResult = true, RuntimeResult = true };
    var viewModel = new MainWindowViewModel(service) { GamePath = @"C:\Steam\DSR" };

    await viewModel.InitializeCommand.ExecuteAsync(null);

    Assert.False(viewModel.CanLaunch);
    Assert.Equal("External runtime is ready. Launch stays locked until dedicated-save and online-blocking safety is installed.", viewModel.Status);
    Assert.False(viewModel.IsBusy);
}

[Fact]
public async Task InitializeCommand_FailureLeavesLaunchDisabled()
{
    var service = new FakeLauncherService { VerifyResult = true, RuntimeException = new IOException("copy failed") };
    var viewModel = new MainWindowViewModel(service) { GamePath = @"C:\Steam\DSR" };

    await viewModel.InitializeCommand.ExecuteAsync(null);

    Assert.False(viewModel.CanLaunch);
    Assert.Equal("Runtime creation failed: copy failed", viewModel.Status);
}
```

- [ ] **Step 2: Run focused tests to verify failure**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~MainWindowViewModelTests
```

Expected: FAIL because the WPF project, service, commands, and view model do not exist.

- [ ] **Step 3: Implement the smallest usable UI**

Create a fixed minimum-size window with:

```xml
<StackPanel Margin="24">
    <TextBlock Text="Dark Souls Remastered installation" />
    <TextBox Text="{Binding GamePath, UpdateSourceTrigger=PropertyChanged}" />
    <Button Content="Verify installation" Command="{Binding VerifyCommand}" />
    <Button Content="Create external runtime" Command="{Binding InitializeCommand}" />
    <ProgressBar Minimum="0" Maximum="100" Value="{Binding ProgressPercent}" />
    <Button Content="Launch Random Dark Souls (safety components required)" IsEnabled="{Binding CanLaunch}" />
    <TextBlock Text="{Binding Status}" TextWrapping="Wrap" />
</StackPanel>
```

The window must say explicitly: `The original game and installed Overhaul are read-only and will not be modified.` It must also explain that this foundation release cannot start the game because dedicated-save redirection and official-online blocking are not installed. `CanLaunch` remains false in every foundation-phase state. Disable all mutation commands while `IsBusy` is true. Catch expected exceptions in the view model and show concise status text; send stack traces only to the external log directory.

- [ ] **Step 4: Run focused and full tests**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~MainWindowViewModelTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS.

- [ ] **Step 5: Build the launcher**

```powershell
dotnet build src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release
```

Expected: build succeeds with zero warnings and produces the WPF launcher under `bin/Release/net8.0-windows`.

- [ ] **Step 6: Commit and push**

```powershell
git add -- DSR-Randomizer.sln src/DSRRandomizer.Launcher tests/DSRRandomizer.Launcher.Tests
git commit -m "feat: add isolated runtime launcher UI"
git push
```

---

### Task 6: Immutability Proof and Release-Content Guard

**Files:**
- Create: `src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs`
- Create: `tests/DSRRandomizer.Foundation.Tests/Packaging/ReleaseContentGuardTests.cs`
- Create: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs`
- Create: `scripts/Test-OriginalInstallUnchanged.ps1`
- Modify: `src/DSRRandomizer.Launcher/LauncherApplication.cs`

**Interfaces:**
- Produces: `ReleaseContentGuard.Validate(IEnumerable<string> relativePaths) : IReadOnlyList<string>` returning prohibited paths
- Consumes: launcher CLI commands `--initialize-runtime <game-path>` and `--validate-package <directory>`

- [ ] **Step 1: Write failing release-guard tests**

```csharp
[Theory]
[InlineData("DarkSoulsRemastered.exe")]
[InlineData("runtime/map/m10_00_00_00.msb.dcx")]
[InlineData("saves/DRAKS-RANDOM.rsl2")]
[InlineData("local-data/game-catalog.json")]
public void Validate_RejectsGameDerivedContent(string path)
{
    Assert.Contains(path, new ReleaseContentGuard().Validate(new[] { path }));
}

[Fact]
public void Validate_AllowsPublishedLauncherAndProjectNotices()
{
    var paths = new[] { "DSRRandomizer.Launcher.exe", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md" };
    Assert.Empty(new ReleaseContentGuard().Validate(paths));
}
```

The guard distinguishes the project-built launcher by exact packaged filename; every other `.exe` and every `.dll` is prohibited in this foundation release.
Add a launcher test that runs `--validate-package <temporary-directory>` with a prohibited fake `DarkSoulsRemastered.exe`, asserts exit code `6`, and asserts the JSON output names that path.

- [ ] **Step 2: Run focused tests to verify failure**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~ReleaseContentGuardTests
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~LauncherApplicationPackageTests
```

Expected: both commands FAIL because `ReleaseContentGuard` and the package-validation command do not exist.

- [ ] **Step 3: Implement the allowlist guard and immutability script**

The guard permits exactly:

```text
DSRRandomizer.Launcher.exe
DSRRandomizer.Launcher.pdb
README.md
LICENSE
THIRD_PARTY_NOTICES.md
CHANGELOG.md
```

No wildcard or publish-manifest exception is permitted. The single-file publish must contain no loose DLL. The guard rejects paths containing `runtime`, `active-seed`, `saves`, `logs`, or `local-data`; save extensions; FromSoftware archive/data extensions; `DarkSoulsRemastered.exe`; every `.dll`; and every unrecognized entry.

Wire `LauncherApplication --validate-package <directory>` to enumerate relative files, call the guard, emit JSON, and exit `6` if any path is prohibited. `scripts/Test-OriginalInstallUnchanged.ps1` accepts `-GamePath` and `-LauncherPath`. It canonicalizes `GamePath`, creates sorted SHA-256 snapshots before and after invoking `--initialize-runtime`, compares relative path, length, timestamp, and hash, writes comparison output under the repository's ignored `artifacts/` directory, and exits `1` on any difference.

- [ ] **Step 4: Run focused and full tests**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj --filter FullyQualifiedName~ReleaseContentGuardTests
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj --filter FullyQualifiedName~LauncherApplicationPackageTests
dotnet test DSR-Randomizer.sln
```

Expected: all tests PASS.

- [ ] **Step 5: Commit and push**

```powershell
git add -- src/DSRRandomizer.Foundation/Packaging src/DSRRandomizer.Launcher/LauncherApplication.cs tests/DSRRandomizer.Foundation.Tests/Packaging tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs scripts/Test-OriginalInstallUnchanged.ps1
git commit -m "test: prove source installation immutability"
git push
```

---

### Task 7: CI, Packaging, External-Runtime Construction Test, and Alpha Release

**Files:**
- Create: `.github/workflows/ci.yml`
- Create: `packaging/package.ps1`
- Create: `CHANGELOG.md`
- Create: `THIRD_PARTY_NOTICES.md`
- Modify: `README.md`
- Modify: `LICENSE`

**Interfaces:**
- Consumes: solution tests, `ReleaseContentGuard`, launcher publish output, and `scripts/Test-OriginalInstallUnchanged.ps1`
- Produces: `artifacts/DSR-Randomizer-v0.1.0-alpha.1-win-x64.zip`
- Produces: `artifacts/DSR-Randomizer-v0.1.0-alpha.1-win-x64.zip.sha256`

- [ ] **Step 1: Add CI with exact build gates**

Create a Windows workflow triggered by pushes and pull requests:

```yaml
name: ci
on:
  push:
  pull_request:
jobs:
  build-test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-dotnet@v4
        with:
          dotnet-version: 8.0.x
      - run: dotnet restore DSR-Randomizer.sln
      - run: dotnet build DSR-Randomizer.sln -c Release --no-restore
      - run: dotnet test DSR-Randomizer.sln -c Release --no-build
      - run: dotnet publish src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release -r win-x64 --self-contained true -o artifacts/publish
      - shell: pwsh
        run: ./packaging/package.ps1 -Version 0.1.0-alpha.1 -PublishPath artifacts/publish -OutputPath artifacts
      - uses: actions/upload-artifact@v4
        with:
          name: DSR-Randomizer-win-x64
          path: |
            artifacts/*.zip
            artifacts/*.sha256
```

- [ ] **Step 2: Implement deterministic packaging and documentation**

`package.ps1` accepts `-Version`, `-PublishPath`, and `-OutputPath`, enumerates publish files, invokes `DSRRandomizer.Launcher.exe --validate-package <PublishPath>`, stops on nonzero exit, copies the allowed files and notices into a clean staging directory, creates the versioned ZIP, and writes lowercase SHA-256 plus two spaces plus filename to `.sha256`.

`CHANGELOG.md` records the isolated launcher, read-only verifier, full-copy runtime, atomic generation pointer, CLI, UI, immutability proof, and absence of gameplay randomization in this alpha. Replace the short `LICENSE` notice with the complete unmodified GNU GPL version 3 text and retain the project copyright/SPDX statement in `README.md`. `THIRD_PARTY_NOTICES.md` reproduces the applicable license and notice text from the exact .NET 8 runtime and Windows Desktop runtime packs used by the self-contained publish, and records xUnit as a test-only dependency. `README.md` adds prerequisites, the approximate 9 GB requirement, initialization instructions, the safety promise, and a warning that `v0.1.0-alpha.1` is an isolation foundation that deliberately cannot start the game.

- [ ] **Step 3: Run all automated verification**

```powershell
dotnet restore DSR-Randomizer.sln
dotnet build DSR-Randomizer.sln -c Release --no-restore
dotnet test DSR-Randomizer.sln -c Release --no-build
dotnet publish src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release -r win-x64 --self-contained true -o artifacts/publish
./packaging/package.ps1 -Version 0.1.0-alpha.1 -PublishPath artifacts/publish -OutputPath artifacts
```

Expected: every command exits `0`; tests report zero failures; ZIP and checksum exist; the ZIP contains no game-derived file.

- [ ] **Step 4: Prove the user's original and Overhaul installation is unchanged**

Run against the actual installation:

```powershell
./scripts/Test-OriginalInstallUnchanged.ps1 `
  -GamePath 'C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED' `
  -LauncherPath './artifacts/publish/DSRRandomizer.Launcher.exe'
```

Expected: exit `0`, `changedFiles: 0`, and a completed external runtime under `%LOCALAPPDATA%\DSR-Randomizer`.

- [ ] **Step 5: Perform the external-runtime construction and launch-lock smoke test**

Run:

```powershell
./artifacts/publish/DSRRandomizer.Launcher.exe --status
./artifacts/publish/DSRRandomizer.Launcher.exe --launch
```

Verify manually and record in `artifacts/smoke-test.md`:

1. `--status` exits `0`, reports a ready runtime below `%LOCALAPPDATA%\DSR-Randomizer\runtimes`, and reports no copied Overhaul or companion paths.
2. The copied runtime contains `DarkSoulsRemastered.exe` but contains no installed `d3d11.dll`, `d3d11_mod.ini`, `overhaul`, `DSRQuickSummonCompanion.dll`, local logs, or credentials.
3. `--launch` exits `2` and no `DarkSoulsRemastered.exe` process starts.
4. Re-run the immutability script and confirm `changedFiles: 0`.

Do not test whether Steam can start the copied executable in this phase. That feasibility test belongs to the runtime-safety plan and may run only after dedicated-save redirection and official-online blocking pass their fatal preflight checks.

- [ ] **Step 6: Commit and push the release foundation**

```powershell
git add -- .github/workflows/ci.yml packaging/package.ps1 CHANGELOG.md THIRD_PARTY_NOTICES.md README.md LICENSE
git commit -m "build: package isolated launcher alpha"
git push
```

- [ ] **Step 7: Review and merge the implementation branch**

Run `superpowers:requesting-code-review`. Resolve all blocking findings, rerun Task 7 Steps 3 through 5, then merge the reviewed branch into `main` without bypassing failing CI.

- [ ] **Step 8: Tag and publish the verified alpha**

```powershell
git switch main
git pull --ff-only
git tag -a v0.1.0-alpha.1 -m "DSR Randomizer isolated launcher foundation"
git push origin v0.1.0-alpha.1
```

Create the GitHub Release from the annotated tag and attach only the ZIP and checksum generated from the verified main commit. Release notes must state that no gameplay randomization is included yet, that the build proves external-runtime isolation, and that game launch is intentionally locked pending dedicated-save and online-blocking safety.

---

## Follow-on Plan Boundaries

After `v0.1.0-alpha.1` passes the external-runtime construction and launch-lock smoke test, write and approve these separate plans in order:

1. `2026-08-24-item-progression-randomizer.md` — local catalog import, seed format, strict item permutation, shops/drops/gifts, progression graph, atomic active-seed output.
2. `2026-08-24-enemy-boss-randomizer.md` — weighted regular enemies with replacement, elite-weight Gravelord Black Phantoms, ordinary-weight Vagrants, compatible boss permutation, AI/SFX/events, and first-visit tutorial scaling.
3. `2026-08-24-runtime-safety-features.md` — native runtime injection, auto-equip, dedicated `.rsl2` save, seed binding, fatal official-online blocking preflight, the first permitted copied-game process start, and Steam/external-runtime feasibility evidence.
4. `2026-08-24-integration-and-stable-release.md` — end-to-end seed stress, real-game encounter matrix, release archive audit, beta, and `v1.0.0` readiness.

Each follow-on plan consumes only the documented artifacts and interfaces from the preceding released phase. No follow-on phase may weaken the source-installation write boundary or copy game-derived files into Git or release archives.

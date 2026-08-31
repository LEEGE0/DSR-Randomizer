# DSR for MOD Redistributable Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify `DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip` containing the project-owned launcher, guard, RMM bridge, bridge host, notices, and Korean installation guide without game files, personal data, or third-party randomizer executables.

**Architecture:** The official publish pins the guard, compatibility profile, bridge DLL, and self-contained bridge host identities into the launcher. A focused installer copies the packaged bridge pair into a selected external root and verifies it before launch. An exact allowlist, deterministic ZIP builder, extracted-ZIP revalidation, and clean-root tests enforce the redistribution boundary.

**Tech Stack:** .NET 8 / C# / WPF / xUnit, C++20 / CMake / CTest, PowerShell 7, Windows x64

**Spec:** `docs/superpowers/specs/2026-08-31-redistributable-release-design.md`

## Global Constraints

- Preserve every pre-existing tracked and untracked change in the dirty `feat/official-online-guard` worktree.
- Never copy from `D:\DSRMOD` into the repository or release.
- Never include Dark Souls Remastered executables/assets, `.sl2`, `.rmm`, Steam IDs, logs, profiles, staging data, generated seeds, spoilers, Item/Enemy Randomizer executables, Mod Engine, or `DS1HeapPatch.dll`.
- Package exactly the 12 paths listed in the spec; PDB files are prohibited.
- Production pinned executable verification must not gain an environment, command-line, or external-file bypass.
- Bridge installation must not require a runtime pointer, save/profile state, Steam ID, or third-party randomizer installation.
- Use version `0.1.0-alpha.2` and output `artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip` plus `.sha256`.
- Do not claim full success unless all 385 managed tests, all 15 native tests, clean-root tests, staged-package validation, extracted-ZIP validation, privacy scan, and checksum verification pass freshly.

---

### Task 1: Restore a truthful native Release test baseline

**Files:**
- Modify: `native/CMakeLists.txt`
- Modify: `native/fixtures/RmmBridgeLoaderFixture.cpp`
- Modify: `native/runtime/profile/PinnedCompatibilityProfile.cpp`
- Modify: `native/runtime/profile/PinnedCompatibilityProfile.h`
- Modify: `native/tests/RunRmmBridgeIntegration.ps1`
- Modify: `scripts/publish-rmm-bridge.ps1`
- Create: `scripts/RmmBridgeDeploymentState.psm1`
- Create: `scripts/tests/Test-RmmBridgeDeploymentState.ps1`
- Test: `native/tests/RmmBridgeConfigurationTests.cpp`

**Interfaces:**
- Consumes: production `BuildPinnedSaveCallsiteProfile(SaveCallsiteRedirectConfiguration&)` and the existing synthetic loader integration flow.
- Produces: a test-only bridge filename used only by `RmmBridgeIntegrationTests`; strict production profile behavior remains unchanged; publisher recovery behavior matches `cleanExit` semantics.

- [ ] **Step 1: Add a failing publisher-recovery regression test**

Create `scripts/tests/Test-RmmBridgeDeploymentState.ps1` as a self-contained assertion script. It creates a temporary `.rmm` and fixture metadata containing boolean `cleanExit=false` with a deliberately stale `lastKnownSha256`, calls the exported validation helper, and asserts acceptance. Add paired cases that reject boolean `cleanExit=true` with the same mismatch and reject string `"false"`.

```powershell
$metadata = @{
    schemaVersion = 1
    steamId = '100000001'
    fixedLength = 4326608
    lastKnownSha256 = ('0' * 64)
    cleanExit = $false
} | ConvertTo-Json -Compress
```

- [ ] **Step 2: Run the new recovery case and confirm it fails for unconditional hash rejection**

Run:

```powershell
pwsh -NoProfile -File scripts/tests/Test-RmmBridgeDeploymentState.ps1
```

Expected: the stale `cleanExit=false` case fails because the current publisher still applies unconditional hash rejection.

- [ ] **Step 3: Make `publish-rmm-bridge.ps1` parse `cleanExit` strictly and apply the recovery rule**

Move the metadata parsing and hash decision into `scripts/RmmBridgeDeploymentState.psm1`, imported by the publisher and test script. The exported helper returns both stored identity and exit state and uses the raw JSON property's runtime type, not PowerShell truthiness.

```powershell
function Read-SaveMetadataState {
    param([Parameter(Mandatory = $true)][string]$Path)
    $metadata = Read-StrictJson $Path
    if ($metadata.cleanExit -isnot [bool]) {
        throw 'save-metadata.json cleanExit must be a JSON boolean.'
    }
    [pscustomobject]@{
        LastKnownSha256 = [string]$metadata.lastKnownSha256
        CleanExit = [bool]$metadata.cleanExit
    }
}
```

Compute the current hash unconditionally. Reject calculation failure. Reject mismatch only when `CleanExit` is true; carry the actual hash into the state returned to later deployment verification.

- [ ] **Step 4: Add a fixture-only callsite contract and a test-only profile build**

Export a fixture-only function from `RmmBridgeLoaderFixture.cpp` that returns the two callsite addresses, expected instruction bytes, and path-argument indexes used by `SaveCallsiteRedirect`. Compile a distinct integration bridge target with `DSR_RANDOMIZER_RMM_BRIDGE_INTEGRATION_PROFILE=1`. Under that compile definition only, `BuildPinnedSaveCallsiteProfile` resolves the fixture export from the main module and populates the configuration after exact byte checks. Without the definition, retain the existing length/SHA/PE/mapped-file verification verbatim.

```cpp
#if defined(DSR_RANDOMIZER_RMM_BRIDGE_INTEGRATION_PROFILE)
    return BuildIntegrationSaveCallsiteProfile(configuration);
#else
    return BuildProductionSaveCallsiteProfile(configuration);
#endif
```

The test DLL must have a distinct output name such as `DSRRandomizer.RmmBridge.Integration.dll` and must not be a dependency or content item of any release target.

- [ ] **Step 5: Point only `RmmBridgeIntegrationTests` at the test bridge**

Change the CTest command's `-BridgeDll` argument to `$<TARGET_FILE:DSRRandomizer.RmmBridge.Integration>` and keep the existing host-ready, callsite-installed, `.rmm` marker, normal-save denial, and no-virtual-save assertions.

- [ ] **Step 6: Run focused and full native Release verification**

Run:

```powershell
ctest --test-dir native/out/build/windows-x64-release -C Release -R "RmmBridge(Configuration|Integration)Tests" --output-on-failure
pwsh -NoProfile -File scripts/tests/Test-RmmBridgeDeploymentState.ps1
pwsh -NoProfile -File scripts/build-native.ps1 -Configuration Release -Test
```

Expected: the focused tests pass and CTest reports 15/15 passed.

- [ ] **Step 7: Commit the native baseline repair**

```powershell
git add native/CMakeLists.txt native/fixtures/RmmBridgeLoaderFixture.cpp native/runtime/profile/PinnedCompatibilityProfile.cpp native/runtime/profile/PinnedCompatibilityProfile.h native/tests/RunRmmBridgeIntegration.ps1 native/tests/RmmBridgeConfigurationTests.cpp scripts/RmmBridgeDeploymentState.psm1 scripts/tests/Test-RmmBridgeDeploymentState.ps1 scripts/publish-rmm-bridge.ps1
git commit -m "test: restore rmm bridge release verification"
```

---

### Task 2: Extend the immutable Release artifact contract

**Files:**
- Modify: `src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj`
- Modify: `src/DSRRandomizer.Launcher/Safety/LaunchArtifactIdentities.cs`
- Modify: `src/DSRRandomizer.Launcher/Safety/ReleaseArtifactIdentityValidator.cs`
- Modify: `src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Safety/LaunchArtifactIdentitiesTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs`
- Modify: `tests/DSRRandomizer.Foundation.Tests/Packaging/ReleaseContentGuardTests.cs`

**Interfaces:**
- Consumes: native Release bridge output and a `PinnedBridgeHostPath` property supplied by the official release orchestration.
- Produces: `LaunchArtifactIdentities(string GuardSha256, string ProfileSha256, string BridgeSha256, string HostSha256)` and exact package validation for all 12 paths.

- [ ] **Step 1: Write failing identity and allowlist tests**

Update `LaunchArtifactIdentitiesTests` to compare four embedded hashes with files beside the launcher. Extend package fixtures with:

```text
INSTALL_KO.md
components/rmm-bridge/DSRRandomizer.RmmBridge.dll
components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe
components/rmm-bridge/deployment-manifest.json
```

Add rejection tests for a missing/tampered DLL, missing/tampered host, mismatched manifest, any `.pdb`, `DarkSoulsRemastered.exe`, `DRAKS0005.sl2`, `DRAKS0005.rmm`, `DS1EnemyRandomizer.exe`, `DarkSoulsItemRandomizer.exe`, `modengine2_launcher.exe`, `DS1HeapPatch.dll`, `seed`, `spoiler`, `logs`, `profile`, `saves`, and `staging` paths.

- [ ] **Step 2: Run the focused managed tests and confirm the new cases fail**

```powershell
dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj -c Release --no-restore --filter FullyQualifiedName~ReleaseContentGuardTests
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter "FullyQualifiedName~LaunchArtifactIdentitiesTests|FullyQualifiedName~LauncherApplicationPackageTests"
```

Expected: failures name the missing bridge identities and newly required package paths.

- [ ] **Step 3: Extend `LaunchArtifactIdentities` and project metadata**

Use exact metadata keys:

```csharp
private const string BridgeKey = "DSRRandomizer.RmmBridgeSha256";
private const string HostKey = "DSRRandomizer.RmmBridgeHostSha256";
```

Add bridge and host properties to the record and validate all four as 64 hexadecimal characters. In the launcher project, add `PinnedBridgePath` and externally overridable `PinnedBridgeHostPath`. Add a `ProjectReference` to `DSRRandomizer.RmmBridgeHost.csproj` with `ReferenceOutputAssembly="false"` so ordinary test builds produce a host apphost before launcher identity generation. Default `PinnedBridgeHostPath` to that configuration's apphost; the official publish overrides it with the self-contained single-file host. The build target fails if either bridge input is missing. Add both files as publish content linked under `components\rmm-bridge` and excluded from the launcher single file.

- [ ] **Step 4: Extend exact content and identity validation**

Make the 12-path spec list the only release allowlist. Remove `DSRForMod.Launcher.pdb`. In `ReleaseArtifactIdentityValidator`, lease and hash the bridge DLL and host, then parse `deployment-manifest.json` with strict case-sensitive property names and JSON value types. Require schema 1, configuration `Release`, and lowercase manifest hashes equal to embedded identities.

- [ ] **Step 5: Run the focused tests to green**

Run the commands from Step 2. Expected: both projects pass with zero failures.

- [ ] **Step 6: Commit the Release contract**

```powershell
git add src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj src/DSRRandomizer.Launcher/Safety/LaunchArtifactIdentities.cs src/DSRRandomizer.Launcher/Safety/ReleaseArtifactIdentityValidator.cs src/DSRRandomizer.Foundation/Packaging/ReleaseContentGuard.cs tests/DSRRandomizer.Launcher.Tests/Safety/LaunchArtifactIdentitiesTests.cs tests/DSRRandomizer.Launcher.Tests/LauncherApplicationPackageTests.cs tests/DSRRandomizer.Foundation.Tests/Packaging/ReleaseContentGuardTests.cs
git commit -m "feat: pin redistributable bridge artifacts"
```

---

### Task 3: Add an idempotent bridge bundle installer

**Files:**
- Create: `src/DSRRandomizer.Launcher/Services/IRmmBridgeBundleInstaller.cs`
- Create: `src/DSRRandomizer.Launcher/Services/RmmBridgeBundleInstaller.cs`
- Create: `tests/DSRRandomizer.Launcher.Tests/Services/RmmBridgeBundleInstallerTests.cs`

**Interfaces:**
- Consumes: a package root, `LaunchArtifactIdentities`, and an external root.
- Produces: `RmmBridgeInstallResult EnsureInstalled(string externalRoot)` with `IsReady`, `Changed`, and `ErrorCode`.

- [ ] **Step 1: Write installer tests against fresh temporary roots**

Cover: fresh install; matching no-op; stale pair replacement; missing source; source hash mismatch; malformed/mismatched manifest; destination reparse point; external-root escape; interrupted mixed pair repaired on the next call; and preservation of `components/rmm-bridge/content/overhaul/GameParam.parambnd.dcx`.

Use only generated temporary bytes and matching SHA-256 values. No test may reference `D:\DSRMOD`, a runtime pointer, a save, or a Steam ID.

- [ ] **Step 2: Run the installer tests and confirm missing types fail compilation**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter FullyQualifiedName~RmmBridgeBundleInstallerTests
```

- [ ] **Step 3: Implement the focused installer contract**

Create:

```csharp
internal sealed record RmmBridgeInstallResult(bool IsReady, bool Changed, string? ErrorCode)
{
    public static RmmBridgeInstallResult Ready(bool changed) => new(true, changed, null);
    public static RmmBridgeInstallResult Failed(string errorCode) => new(false, false, errorCode);
}

internal interface IRmmBridgeBundleInstaller
{
    RmmBridgeInstallResult EnsureInstalled(string externalRoot);
}
```

`RmmBridgeBundleInstaller` uses `LaunchArtifactLease` for packaged inputs, validates the manifest before writes, canonicalizes every path, rejects reparse ancestors, writes sibling temporary files with write-through flush, replaces DLL and host, writes the manifest last, and reopens all three installed files for final verification. Catch only expected I/O, access, path, and JSON failures and map them to the three spec error codes.

- [ ] **Step 4: Run installer tests to green and check the package-content preservation case**

Run the Step 2 command. Expected: every installer test passes.

- [ ] **Step 5: Commit the installer**

```powershell
git add src/DSRRandomizer.Launcher/Services/IRmmBridgeBundleInstaller.cs src/DSRRandomizer.Launcher/Services/RmmBridgeBundleInstaller.cs tests/DSRRandomizer.Launcher.Tests/Services/RmmBridgeBundleInstallerTests.cs
git commit -m "feat: install pinned rmm bridge bundle"
```

---

### Task 4: Integrate bridge installation into the guarded launch flow

**Files:**
- Modify: `src/DSRRandomizer.Launcher/Services/LauncherService.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Services/LauncherServiceTests.cs`
- Modify: `tests/DSRRandomizer.Launcher.Tests/Services/RandomizerRuntimeIntegrationTests.cs`

**Interfaces:**
- Consumes: `IRmmBridgeBundleInstaller.EnsureInstalled(externalRoot)` from Task 3.
- Produces: launch preflight that guarantees an installed, hash-verified bridge pair before bridged TOML generation or Mod Engine process creation.

- [ ] **Step 1: Add failing launch-order and failure-code tests**

Inject a recording fake installer through the internal `LauncherService` constructor. Assert:

- installation runs after the external launch gate is acquired and before the bridge artifact lease/configuration generation;
- each installer error code is returned unchanged;
- no Mod Engine process starts on installer failure;
- a fresh external root receives the pair without any save/profile dependency;
- successful installation still yields exactly one bridge DLL entry and the required heap patch in the bridged TOML.

- [ ] **Step 2: Run the focused launcher service tests and confirm failure**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter "FullyQualifiedName~LauncherServiceTests|FullyQualifiedName~RandomizerRuntimeIntegrationTests"
```

- [ ] **Step 3: Wire the installer into constructors and `LaunchModdedAsync`**

The public constructor builds `RmmBridgeBundleInstaller` from `AppContext.BaseDirectory` and embedded identities. The internal constructor accepts `IRmmBridgeBundleInstaller`. Immediately before opening the installed bridge artifact, call:

```csharp
var bridgeInstall = _bridgeInstaller.EnsureInstalled(_externalRoot);
if (!bridgeInstall.IsReady)
{
    return SafetyLaunchResult.Failed(bridgeInstall.ErrorCode!);
}
```

Do not move save preparation earlier and do not make the installer read randomizer or save state.

- [ ] **Step 4: Run focused tests and the full managed suite**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter "FullyQualifiedName~LauncherServiceTests|FullyQualifiedName~RandomizerRuntimeIntegrationTests|FullyQualifiedName~RmmBridgeBundleInstallerTests"
dotnet test DSR-Randomizer.sln -c Release --no-restore
```

Expected: focused tests and all managed tests pass.

- [ ] **Step 5: Commit launch integration**

```powershell
git add src/DSRRandomizer.Launcher/Services/LauncherService.cs tests/DSRRandomizer.Launcher.Tests/Services/LauncherServiceTests.cs tests/DSRRandomizer.Launcher.Tests/Services/RandomizerRuntimeIntegrationTests.cs
git commit -m "feat: provision bridge before modded launch"
```

---

### Task 5: Build and validate the deterministic redistributable ZIP

**Files:**
- Create: `packaging/build-release.ps1`
- Modify: `packaging/package.ps1`
- Modify: `tests/DSRRandomizer.Launcher.Tests/PinnedArtifactPublishTests.cs`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: native Release outputs, bridge-host project, launcher project, exact package allowlist, and version `0.1.0-alpha.2`.
- Produces: the final ZIP/checksum names from the spec and validation of both staging and extracted ZIP contents.

- [ ] **Step 1: Add failing packaging integration assertions**

Extend `PinnedArtifactPublishTests` to assert:

- the official release publish contains the bridge DLL, host, and manifest;
- a future-dated tampered bridge or host is replaced by a new official publish;
- the ZIP contains exactly the 12 allowlisted entries and no PDB;
- the extracted ZIP passes `--validate-package`;
- a duplicate, rooted, or `../` ZIP entry is rejected by the archive validator;
- the checksum sidecar equals a fresh SHA-256 of the ZIP.

- [ ] **Step 2: Run the packaging integration test and confirm the current pipeline fails**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter FullyQualifiedName~PinnedArtifactPublishTests
```

- [ ] **Step 3: Implement `build-release.ps1` orchestration**

The script accepts only a validated version and output root, then runs:

```powershell
$releaseWork = Join-Path $repositoryRoot "artifacts/release-work-$Version"
$hostPublish = Join-Path $releaseWork 'rmm-bridge-host'
$launcherPublish = Join-Path $releaseWork 'launcher'
$bridgeDll = Join-Path $repositoryRoot 'native/out/build/windows-x64-release/native/runtime/Release/DSRRandomizer.RmmBridge.dll'
pwsh -NoProfile -File scripts/build-native.ps1 -Configuration Release -Test
dotnet publish src/DSRRandomizer.RmmBridgeHost/DSRRandomizer.RmmBridgeHost.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o $hostPublish
dotnet publish src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PinnedBridgePath=$bridgeDll -p:PinnedBridgeHostPath=(Join-Path $hostPublish 'DSRRandomizer.RmmBridgeHost.exe') -o $launcherPublish
```

Use validated output descendants under `artifacts/release-work-0.1.0-alpha.2`; never use `D:\DSRMOD`. Pass the exact generated launcher dependency-manifest path into `package.ps1` instead of asking it to choose the newest `obj/Release` file.

- [ ] **Step 4: Extend `package.ps1` and archive revalidation**

Stage exactly the 12 paths, never copy a PDB, write the strict bridge manifest, run the staged launcher validator, and create sorted fixed-timestamp entries. Before extracting, normalize each `ZipArchiveEntry.FullName` and reject empty, rooted, duplicate, backslash-aliased, dot, or dot-dot paths. Extract into a unique validated temporary descendant, run the packaged launcher validator there, then compute the checksum.

Name outputs exactly:

```powershell
$zipName = "DSR-for-MOD-v$Version-win-x64.zip"
$checksumPath = "$zipPath.sha256"
```

- [ ] **Step 5: Update CI to use the official release path**

Replace the standalone launcher publish/package steps with:

```yaml
- shell: pwsh
  run: ./packaging/build-release.ps1 -Version 0.1.0-alpha.2 -OutputPath artifacts
```

Upload `artifacts/DSR-for-MOD-*.zip` and matching `.sha256` files.

- [ ] **Step 6: Run packaging integration and full Release verification**

```powershell
dotnet test tests/DSRRandomizer.Launcher.Tests/DSRRandomizer.Launcher.Tests.csproj -c Release --no-restore --filter FullyQualifiedName~PinnedArtifactPublishTests
dotnet test DSR-Randomizer.sln -c Release --no-restore
pwsh -NoProfile -File scripts/build-native.ps1 -Configuration Release -Test
```

- [ ] **Step 7: Commit the release pipeline**

```powershell
git add packaging/build-release.ps1 packaging/package.ps1 tests/DSRRandomizer.Launcher.Tests/PinnedArtifactPublishTests.cs .github/workflows/ci.yml
git commit -m "build: package verified redistributable release"
```

---

### Task 6: Publish recipient documentation and the final verified artifacts

**Files:**
- Create: `INSTALL_KO.md`
- Modify: `README.md`
- Modify: `THIRD_PARTY_NOTICES.md`
- Modify: `CHANGELOG.md`
- Modify: `HANDOFF_DISTRIBUTION_2026-08-31.md`
- Output: `artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip`
- Output: `artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip.sha256`

**Interfaces:**
- Consumes: official Item/Enemy Randomizer URLs and the verified pipeline from Task 5.
- Produces: recipient-facing Korean setup instructions, truthful release notes, final ZIP, and matching checksum.

- [ ] **Step 1: Write the Korean installation guide with official sources and exact exclusions**

Use these official URLs:

```text
https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases
https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files
```

State that neither tool, the Enemy Randomizer's Mod Engine fork, nor `DS1HeapPatch.dll` is included. Explain the copied-runtime directory layout already recognized by `RandomizerRuntimeIntegration.Resolve`, Steam Offline Mode, new external-root selection, runtime initialization, save isolation, and the prohibition on sharing Steam ID/save/seed/spoiler files.

- [ ] **Step 2: Update notices, README, changelog, and handoff truthfully**

Document the bundled project-owned bridge/host and SoulsFormatsNEXT obligations. Record `0.1.0-alpha.2`, bridge auto-install/verification, third-party user-supply boundary, clean-root verification, and native test fixture repair. Do not state that the ZIP includes or installs third-party tools.

- [ ] **Step 3: Run documentation and privacy preflight**

```powershell
rg -n -S "D:\\DSRMOD|C:\\Users\\User|146808034" README.md INSTALL_KO.md THIRD_PARTY_NOTICES.md CHANGELOG.md
rg -n -S "TBD|TODO|implement later" README.md INSTALL_KO.md THIRD_PARTY_NOTICES.md CHANGELOG.md
git diff --check
```

Expected: no personal path/ID matches, no placeholders, and no whitespace errors.

- [ ] **Step 4: Build the final release from the worktree**

```powershell
pwsh -NoProfile -File packaging/build-release.ps1 -Version 0.1.0-alpha.2 -OutputPath artifacts
```

Expected: the exact ZIP and `.sha256` paths from the spec.

- [ ] **Step 5: Independently inspect the final ZIP and checksum**

List every ZIP entry and compare it byte-for-byte with the 12-path allowlist. Extract to a new temporary directory, run the packaged launcher `--validate-package`, and scan all extracted bytes for `D:\DSRMOD`, `C:\Users\User`, and `146808034`. Recompute the ZIP SHA-256 and compare it with the sidecar.

- [ ] **Step 6: Run the final full verification gate**

```powershell
dotnet test DSR-Randomizer.sln -c Release --no-restore
pwsh -NoProfile -File scripts/build-native.ps1 -Configuration Release -Test
```

Expected: 385/385 managed tests and 15/15 native tests pass, plus successful package and checksum checks from Steps 4-5.

- [ ] **Step 7: Commit documentation and release metadata without committing generated ZIPs unless repository policy already tracks them**

```powershell
git add INSTALL_KO.md README.md THIRD_PARTY_NOTICES.md CHANGELOG.md HANDOFF_DISTRIBUTION_2026-08-31.md
git commit -m "docs: publish redistributable setup guide"
```

- [ ] **Step 8: Push the feature branch**

```powershell
git push -u origin feat/official-online-guard
```

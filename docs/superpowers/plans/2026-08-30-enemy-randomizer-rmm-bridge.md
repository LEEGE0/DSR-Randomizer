# Enemy Randomizer RMM Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the enemy randomizer's existing `Launch DS1` button launch the copied game with every logical `DRAKS0005.sl2` operation redirected exclusively to the selected dedicated `DRAKS0005.rmm`.

**Architecture:** Mod Engine 2 loads a project-owned x64 bridge DLL through `external_dlls`; its exported `modengine_ext_init` resolves and validates the active copied runtime, starts a managed session host, waits for a one-time readiness event, then installs the existing native save hooks. The managed host owns the existing `DedicatedSaveService` lease until the game exits, while every bootstrap failure is fail-closed and terminates the copied game before normal-save access is possible.

**Tech Stack:** C++20, Win32, CMake 3.28+, MinHook, .NET 8, xUnit, PowerShell 7

**Spec:** `docs/superpowers/specs/2026-08-29-enemy-randomizer-rmm-bridge-design.md`

## Global Constraints

- Preserve the exact enemy-randomizer `Launch DS1` workflow and its self-contained directory.
- Do not modify or redistribute `DS1EnemyRandomizer.exe` or move it into the copied game root.
- The only permitted mutable root is the resolved external root, currently `D:\DSR MOD`.
- Never read or write a normal/Overhaul `.sl2` as a fallback; missing or invalid bridge state terminates the copied game.
- Require x64 Windows, `DarkSoulsRemastered.exe` as the live process image, runtime-current schema as currently emitted, save metadata schema version `1`, and a dedicated-save length of exactly `4,326,608` bytes.
- Treat the bundled Mod Engine callback ABI as pinned: `extern "C" bool modengine_ext_init(void*, void**)`.
- Perform no file-system, process, synchronization, hook, or managed-runtime work from `DllMain`.
- Reuse `InstallSaveHooks` with `protectFileIo = true` and `diagnosticMode = false`; do not enable network, Steam, heartbeat, game-service, or integrity-monitor protections.
- Preserve the existing guarded-launch path and all existing managed/native tests.
- Back up the generated enemy-randomizer TOML byte-for-byte before the first local edit.
- Do not launch the real game automatically during verification; the user performs the final smoke launch in Steam Offline Mode.

## File map

- `native/runtime/bridge/RmmBridgeConfiguration.{h,cpp}` — bounded JSON parsing, runtime/profile/save path resolution, and file identity validation.
- `native/runtime/bridge/RmmBridgeHostClient.{h,cpp}` — one-time event creation, managed host launch, readiness/timeout handling, and child-process lifetime transfer.
- `native/runtime/bridge/RmmBridgeBootstrap.{h,cpp}` — ordered configuration, host handshake, hook installation, logging, and injectable fail-closed action.
- `native/runtime/bridge/RmmBridgeEntry.cpp` — inert `DllMain` and exported Mod Engine callback only.
- `native/include/DSRRandomizer/RmmBridgeProtocol.h` — exit codes, readiness timeout, and command-line switch names shared by native tests and deployment documentation.
- `src/DSRRandomizer.RmmBridgeHost/` — managed CLI, live-process validation, dedicated-save session coordination, and exit observation.
- `tests/DSRRandomizer.RmmBridgeHost.Tests/` — deterministic host coordinator tests with process/event abstractions.
- `native/fixtures/RmmBridgeLoaderFixture.cpp` — synthetic Mod Engine-style callback loader.
- `native/tests/RmmBridge*Tests.cpp` — native configuration, bootstrap, and end-to-end save redirection tests.
- `scripts/publish-rmm-bridge.ps1` — reproducible Release build/publish/deploy with SHA-256 verification.
- `docs/enemy-randomizer-rmm-bridge.md` — regeneration, verification, rollback, and final smoke-test instructions.

---

### Task 1: Native bridge configuration resolver

**Files:**
- Create: `native/runtime/bridge/RmmBridgeConfiguration.h`
- Create: `native/runtime/bridge/RmmBridgeConfiguration.cpp`
- Create: `native/tests/RmmBridgeConfigurationTests.cpp`
- Modify: `native/runtime/CMakeLists.txt`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: Win32 final-path, file-identity, known-folder, and cryptographic hashing APIs; the on-disk `RuntimePointer`, `SaveProfileCandidate`, and `DedicatedSaveMetadata` JSON contracts.
- Produces: `BridgeConfigurationResult ResolveBridgeConfiguration(const BridgeConfigurationPlatform&)`, where success contains canonical `runtimeRoot`, `externalRoot`, `runtimeId`, `steamId`, `virtualDocuments`, `virtualLogicalSave`, `realSaveRoot`, `externalSaveRoot`, `dedicatedRmm`, `hostExecutable`, `saveIdentity`, and `metadataIdentity`.

- [ ] **Step 1: Add failing resolver tests**

  Create a fake `BridgeConfigurationPlatform` backed by maps for files, canonical paths, file attributes, link counts, lengths, and SHA-256 values. Assert success for a canonical runtime and rejection for: wrong executable leaf, runtime outside `<root>\runtimes`, duplicate/unknown JSON properties, input over 64 KiB, non-decimal Steam ID, path traversal, reparse points, link count other than one, wrong save length, metadata schema other than one, and metadata hash/Steam ID/length mismatch.

  ```cpp
  const auto result = ResolveBridgeConfiguration(platform);
  Require(result.ok, result.message);
  RequireEqual(result.value.dedicatedRmm,
      LR"(D:\DSR MOD\saves\146808034\DRAKS0005.rmm)");

  platform.files[metadataPath] = R"({"schemaVersion":2})";
  const auto unsupported = ResolveBridgeConfiguration(platform);
  Require(!unsupported.ok, "schema 2 must fail closed");
  RequireEqual(unsupported.error, BridgeConfigurationError::UnsupportedMetadataSchema);
  ```

- [ ] **Step 2: Register and run the red test**

  Add `RmmBridgeConfigurationTests` in `native/CMakeLists.txt`, linked to a new static `DSRRandomizer.RmmBridge.Core` target. Run:

  ```powershell
  cmake --preset windows-msvc-debug
  cmake --build --preset windows-msvc-debug --target RmmBridgeConfigurationTests
  ctest --preset windows-msvc-debug -R RmmBridgeConfigurationTests --output-on-failure
  ```

  Expected: compilation fails because `RmmBridgeConfiguration.h` and resolver symbols do not exist.

- [ ] **Step 3: Implement bounded parsing and canonical resolution**

  Define the exact contracts:

  ```cpp
  enum class BridgeConfigurationError {
    None, ProcessImageInvalid, LayoutInvalid, ConfigurationMissing,
    ConfigurationMalformed, ConfigurationTooLarge, RuntimeMismatch,
    SteamIdInvalid, SaveInvalid, MetadataInvalid,
    UnsupportedMetadataSchema, FileAliasRejected
  };

  struct BridgeConfiguration {
    std::wstring runtimeRoot, externalRoot, runtimeId, steamId;
    std::wstring virtualDocuments, virtualLogicalSave, realSaveRoot;
    std::wstring externalSaveRoot, dedicatedRmm, hostExecutable;
    std::string saveIdentity, metadataIdentity;
  };

  struct BridgeConfigurationResult {
    bool ok;
    BridgeConfiguration value;
    BridgeConfigurationError error;
    std::wstring message;
  };
  ```

  Implement `ReadBoundedUtf8(path, 65536)`, a flat-object JSON reader that rejects duplicate keys, malformed UTF-8, trailing data, and invalid escape sequences, and schema-specific readers that reject unexpected members. Canonicalize every existing file/directory through an opened handle, require the runtime pointer's `relativeRuntimePath` to resolve to the live runtime, require the process leaf `DarkSoulsRemastered.exe`, validate the dedicated save as a regular non-reparse single-link file of `4326608` bytes, and compare the lowercase SHA-256 to metadata.

- [ ] **Step 4: Run focused and existing save tests**

  ```powershell
  cmake --build --preset windows-msvc-debug --target RmmBridgeConfigurationTests SavePathPolicyTests SaveHookIntegrationTests
  ctest --preset windows-msvc-debug -R "RmmBridgeConfigurationTests|SavePathPolicyTests|SaveHookIntegrationTests" --output-on-failure
  ```

  Expected: all selected tests pass.

- [ ] **Step 5: Commit the resolver**

  ```powershell
  git add native/runtime/bridge native/runtime/CMakeLists.txt native/tests/RmmBridgeConfigurationTests.cpp native/CMakeLists.txt
  git commit -m "feat: validate rmm bridge configuration"
  ```

---

### Task 2: Managed dedicated-save bridge host

**Files:**
- Create: `src/DSRRandomizer.RmmBridgeHost/DSRRandomizer.RmmBridgeHost.csproj`
- Create: `src/DSRRandomizer.RmmBridgeHost/Program.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/BridgeHostArguments.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/BridgeSessionCoordinator.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/IBridgeHostPlatform.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/WindowsBridgeHostPlatform.cs`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/BridgeHostArgumentsTests.cs`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/BridgeSessionCoordinatorTests.cs`
- Modify: `DSR-Randomizer.sln`

**Interfaces:**
- Consumes: `InstallationSelectionStore.CreateReadOnly`, `WindowsPathCanonicalizer`, `WriteBoundary.Create`, `LocalDataLayout.Create`, `SaveSelectionStore`, `SystemFileAccess`, `DedicatedSaveService.PrepareAsync`, `BeginSessionAsync`, and `CompleteSessionAsync`.
- Produces: `Task<int> BridgeSessionCoordinator.RunAsync(BridgeHostArguments, CancellationToken)` and CLI `--game-pid <uint> --external-root <absolute> --runtime-id <id> --steam-id <digits> --ready-event <Local\\name>`.

- [ ] **Step 1: Add failing argument and coordinator tests**

  Assert strict switch parsing (each switch exactly once, no unknown switches, uint PID, absolute root, runtime ID matching the runtime directory leaf, 1-20 decimal Steam-ID digits, and a `Local\DSRRandomizer.RmmBridge.*` event). With a fake platform and real temporary `DedicatedSaveService` data, assert that readiness is signaled only after metadata is marked unclean and the session file exists; process mismatch and BeginSession failure must return without signaling.

  ```csharp
  var task = coordinator.RunAsync(arguments, CancellationToken.None);
  await platform.ReadySignaled.Task.WaitAsync(TimeSpan.FromSeconds(2));
  var metadata = await fixture.ReadMetadataAsync();
  Assert.False(metadata.CleanExit);
  platform.ExitCode.SetResult(0u);
  Assert.Equal(0, await task);
  Assert.True((await fixture.ReadMetadataAsync()).CleanExit);
  ```

- [ ] **Step 2: Add projects and confirm the red tests**

  Reference `DSRRandomizer.Foundation` from both the host and host tests, and add xUnit/test SDK packages matching the existing test projects. Run:

  ```powershell
  dotnet test tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj -c Debug
  ```

  Expected: tests fail because parser/coordinator behavior is not implemented.

- [ ] **Step 3: Implement strict argument parsing and platform abstraction**

  ```csharp
  internal sealed record BridgeHostArguments(
      uint GamePid,
      string ExternalRoot,
      string RuntimeId,
      string SteamId,
      string ReadyEventName);

  internal interface IBridgeHostPlatform
  {
      string GetProcessImagePath(uint processId);
      EventWaitHandle OpenReadyEvent(string name);
      Task<uint> WaitForExitAsync(uint processId, CancellationToken cancellationToken);
  }
  ```

  The Windows implementation opens the process with query/synchronize rights, resolves its final image path, opens the pre-created event without creating a replacement, waits asynchronously, and obtains the actual exit code.

- [ ] **Step 4: Implement session coordination**

  Independently re-resolve `source-installation.json`, create the write boundary/layout, verify `runtime-current.json` and selected profile against the CLI values and live executable, call `PrepareAsync`, pass its two identities to `BeginSessionAsync`, signal readiness, await game exit, and call `CompleteSessionAsync(..., normalGuardedExit: exitCode == 0, ...)`. Always attempt abnormal completion after post-readiness exceptions so metadata remains unclean and the lease is released.

- [ ] **Step 5: Run host and foundation tests**

  ```powershell
  dotnet test tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj -c Debug
  dotnet test tests/DSRRandomizer.Foundation.Tests/DSRRandomizer.Foundation.Tests.csproj -c Debug --filter "FullyQualifiedName~DedicatedSaveServiceTests|FullyQualifiedName~SaveSelectionStoreTests"
  ```

  Expected: all selected tests pass, including normal exit, nonzero exit, process identity mismatch, concurrent session rejection, and no-ready-on-failure cases.

- [ ] **Step 6: Commit the host**

  ```powershell
  git add DSR-Randomizer.sln src/DSRRandomizer.RmmBridgeHost tests/DSRRandomizer.RmmBridgeHost.Tests
  git commit -m "feat: coordinate rmm bridge save sessions"
  ```

---

### Task 3: Mod Engine bridge callback and fail-closed bootstrap

**Files:**
- Create: `native/include/DSRRandomizer/RmmBridgeProtocol.h`
- Create: `native/runtime/bridge/RmmBridgeHostClient.h`
- Create: `native/runtime/bridge/RmmBridgeHostClient.cpp`
- Create: `native/runtime/bridge/RmmBridgeBootstrap.h`
- Create: `native/runtime/bridge/RmmBridgeBootstrap.cpp`
- Create: `native/runtime/bridge/RmmBridgeEntry.cpp`
- Create: `native/tests/RmmBridgeBootstrapTests.cpp`
- Modify: `native/runtime/CMakeLists.txt`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 `ResolveBridgeConfiguration`, Task 2 host CLI, and existing `InstallSaveHooks(const SaveHookConfiguration&)`.
- Produces: `BridgeBootstrapResult BootstrapRmmBridge(BridgeBootstrapPlatform&)` and exported `extern "C" __declspec(dllexport) bool modengine_ext_init(void*, void**)`.

- [ ] **Step 1: Add failing bootstrap tests**

  Inject fake resolver, host, hook installer, log writer, and terminator functions. Assert strict order `resolve -> start host -> host ready -> install hooks`; hook arguments contain the five canonical paths and exact flags; failures at every stage write one diagnostic and request termination; `DllMain` performs no bootstrap; callback always nulls the extension pointer and returns `false` after successful installation.

  ```cpp
  const auto result = BootstrapRmmBridge(platform);
  Require(result.ok, result.message);
  RequireEqual(platform.calls,
      std::vector<std::string>{"resolve", "start-host", "wait-ready", "install-hooks"});
  Require(platform.hookConfiguration.protectFileIo, "file I/O protection required");
  Require(!platform.hookConfiguration.diagnosticMode, "diagnostic mode must stay off");
  ```

- [ ] **Step 2: Register target/tests and verify red**

  Create shared target `DSRRandomizer.RmmBridge` linked to `DSRRandomizer.RmmBridge.Core` and `DSRRandomizer.Runtime.Core`. Run:

  ```powershell
  cmake --build --preset windows-msvc-debug --target RmmBridgeBootstrapTests DSRRandomizer.RmmBridge
  ctest --preset windows-msvc-debug -R RmmBridgeBootstrapTests --output-on-failure
  ```

  Expected: compilation/link failure for missing bootstrap/client symbols.

- [ ] **Step 3: Implement the host handshake**

  Generate 128 random bits with `BCryptGenRandom`, format the event as `Local\DSRRandomizer.RmmBridge.<32 lowercase hex>`, create a manual-reset event with an explicit owner-only security descriptor, and start the host with fully quoted arguments. Wait at most 15 seconds for either readiness or host exit. Close the readiness event after success but retain the host process handle in a process-lifetime owner so the host cannot be orphaned by premature handle destruction.

  ```cpp
  inline constexpr DWORD kBridgeReadyTimeoutMs = 15'000;
  inline constexpr wchar_t kBridgeHostFileName[] = L"DSRRandomizer.RmmBridgeHost.exe";
  inline constexpr wchar_t kReadyEventPrefix[] = L"Local\\DSRRandomizer.RmmBridge.";
  ```

- [ ] **Step 4: Implement ordered bootstrap and callback**

  Convert the resolved paths into `SaveHookConfiguration`, wait for host readiness before calling `InstallSaveHooks`, and log UTC timestamp/error code/message to `<externalRoot>\logs\rmm-bridge.log`. Production failure calls `TerminateProcess(GetCurrentProcess(), bridgeErrorCode)`; test failure records the requested action. Keep `DllMain` limited to `DisableThreadLibraryCalls`.

  ```cpp
  extern "C" __declspec(dllexport)
  bool modengine_ext_init(void*, void** extension) noexcept {
    if (extension != nullptr) {
      *extension = nullptr;
    }
    const auto result = BootstrapRmmBridge(ProductionBridgePlatform());
    if (!result.ok) {
      FailClosed(result);
    }
    return false;
  }
  ```

- [ ] **Step 5: Run bridge and hook tests**

  ```powershell
  cmake --build --preset windows-msvc-debug --target DSRRandomizer.RmmBridge RmmBridgeBootstrapTests RmmBridgeConfigurationTests SaveHookIntegrationTests
  ctest --preset windows-msvc-debug -R "RmmBridge|SaveHookIntegrationTests" --output-on-failure
  ```

  Expected: all selected tests pass and `dumpbin /exports` lists `modengine_ext_init`.

- [ ] **Step 6: Commit native bootstrap**

  ```powershell
  git add native/include/DSRRandomizer/RmmBridgeProtocol.h native/runtime/bridge native/runtime/CMakeLists.txt native/tests/RmmBridgeBootstrapTests.cpp native/CMakeLists.txt
  git commit -m "feat: bootstrap rmm hooks from mod engine"
  ```

---

### Task 4: Synthetic loader integration and fail-closed regression coverage

**Files:**
- Create: `native/fixtures/RmmBridgeLoaderFixture.cpp`
- Create: `native/tests/RmmBridgeIntegrationTests.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: built bridge DLL, host executable, `SaveClientFixture`, and a temporary external-root/runtime/profile/save fixture.
- Produces: one CTest integration target proving the Mod Engine load/callback order and exclusive `.rmm` access without launching the real game.

- [ ] **Step 1: Add the failing synthetic-loader integration test**

  The fixture accepts bridge path and external root, loads the bridge via `LoadLibraryW`, obtains `modengine_ext_init` with `GetProcAddress`, invokes it with a non-null sentinel extension output, then performs the same Documents/save open/read/write operations as `SaveClientFixture`. The test snapshots normal and Overhaul fixture files, starts the loader from `<root>\runtimes\<id>\DarkSoulsRemastered.exe`, and asserts only the `.rmm` changed.

- [ ] **Step 2: Register and run the red integration test**

  ```powershell
  cmake --build --preset windows-msvc-debug --target RmmBridgeIntegrationTests
  ctest --preset windows-msvc-debug -R RmmBridgeIntegrationTests --output-on-failure
  ```

  Expected: test fails until the fixture deployment layout and host executable override are wired into the bootstrap test platform.

- [ ] **Step 3: Complete deterministic fixture wiring**

  Add a test-only environment contract `DSR_RMM_BRIDGE_TEST_HOST` accepted only when the process image is the synthetic fixture; production `DarkSoulsRemastered.exe` always uses `<externalRoot>\components\rmm-bridge\DSRRandomizer.RmmBridgeHost.exe`. Cover host missing, `.rmm` missing, metadata mismatch, timeout, and forced hook failure; each case must exit before the fixture observes the normal save.

- [ ] **Step 4: Run native Debug and Release suites**

  ```powershell
  cmake --build --preset windows-msvc-debug
  ctest --preset windows-msvc-debug --output-on-failure
  cmake --preset windows-msvc-release
  cmake --build --preset windows-msvc-release
  ctest --preset windows-msvc-release --output-on-failure
  ```

  Expected: every native test passes in both configurations.

- [ ] **Step 5: Commit integration coverage**

  ```powershell
  git add native/fixtures/RmmBridgeLoaderFixture.cpp native/tests/RmmBridgeIntegrationTests.cpp native/CMakeLists.txt
  git commit -m "test: prove mod engine rmm bridge isolation"
  ```

---

### Task 5: Publish, deploy, and configure the current enemy randomizer

**Files:**
- Create: `scripts/publish-rmm-bridge.ps1`
- Create: `docs/enemy-randomizer-rmm-bridge.md`
- Modify: `packaging/package.ps1`
- Local generated file with backup: `D:\DSR MOD\runtimes\runtime-a39cb5e0b3d6c410d550f468b5e034ebe3d4db3e2c719ca3d3cff64102295c10\Mods\DS1 Enemy Randomizer-922-v0-1-3-1778373918\DS1EnemyRandomizer\dist1\config_randomizer.toml`

**Interfaces:**
- Consumes: Release native DLL, self-contained win-x64 managed host publish, the current enemy-randomizer folder, and current `D:\DSR MOD` configuration.
- Produces: verified artifacts under `D:\DSR MOD\components\rmm-bridge`, a backed-up current TOML containing only actual DLLs in `external_dlls`, and repeatable UI/regeneration instructions.

- [ ] **Step 1: Add a publish-script verification mode that initially fails**

  Support `-ExternalRoot`, `-Configuration Release`, and `-VerifyOnly`. `-VerifyOnly` must fail if the bridge export is absent, the host is not win-x64, deployed SHA-256 hashes differ, current runtime pointer/profile/save metadata disagree, TOML omits the bridge DLL, or TOML contains `DarkSoulsItemRandomizer.exe` in `external_dlls`.

  ```powershell
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot 'D:\DSR MOD' -VerifyOnly
  if ($LASTEXITCODE -ne 0) { throw 'RMM bridge verification failed.' }
  ```

  Expected before publish: nonzero exit reporting missing bridge artifacts.

- [ ] **Step 2: Implement reproducible build/publish/deploy**

  Build `DSRRandomizer.RmmBridge` from the Release CMake preset and publish the host using:

  ```powershell
  dotnet publish src/DSRRandomizer.RmmBridgeHost/DSRRandomizer.RmmBridgeHost.csproj `
    -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
  ```

  Stage into a unique directory below the external root, hash every output, then use same-volume atomic replacement into `components\rmm-bridge`. Verify destination hashes after copying and never delete or rewrite files outside the external root.

- [ ] **Step 3: Back up and repair the current generated TOML**

  Resolve the active runtime through `runtime-current.json`, copy `config_randomizer.toml` to `config_randomizer.toml.pre-rmm-bridge-<UTC timestamp>.bak`, preserve all existing Mod Engine heap/loose-file entries, remove `DarkSoulsItemRandomizer.exe` from `external_dlls`, and add the deployed `DSRRandomizer.RmmBridge.dll` exactly once using the TOML's existing path style.

- [ ] **Step 4: Document durable enemy-randomizer UI settings and rollback**

  Record: keep `Select exe` on the copied runtime executable; set the other-mod directory to `D:\DSR MOD\components\rmm-bridge`; enable DLL-mod merge; keep game-directory merge enabled for item-randomizer loose files; after every `Randomize!`, run `-VerifyOnly`. Rollback consists of removing bridge DLL selection, regenerating TOML, and confirming no bridge path remains.

- [ ] **Step 5: Run full managed/native/publish verification**

  ```powershell
  dotnet test DSR-Randomizer.sln -c Release
  cmake --build --preset windows-msvc-release
  ctest --preset windows-msvc-release --output-on-failure
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot 'D:\DSR MOD'
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot 'D:\DSR MOD' -VerifyOnly
  git diff --check
  ```

  Expected: all tests pass; publish and verification exit zero; deployed hashes match; `.rmm` remains `4326608` bytes; normal saves and Steam installation hashes sampled before the test remain unchanged; TOML contains the bridge DLL and not the item-randomizer executable.

- [ ] **Step 6: Commit publishing and operations documentation**

  ```powershell
  git add scripts/publish-rmm-bridge.ps1 packaging/package.ps1 docs/enemy-randomizer-rmm-bridge.md
  git commit -m "build: publish enemy randomizer rmm bridge"
  ```

---

### Task 6: Final non-game audit and handoff

**Files:**
- Verify only: all committed files and deployed artifacts from Tasks 1-5

**Interfaces:**
- Consumes: complete bridge implementation and local deployment.
- Produces: evidence-backed handoff for the user's one manual Steam Offline Mode `Launch DS1` smoke test.

- [ ] **Step 1: Run clean-tree and artifact audit**

  ```powershell
  git status --short
  git log --oneline -6
  Get-FileHash 'D:\DSR MOD\components\rmm-bridge\DSRRandomizer.RmmBridge.dll' -Algorithm SHA256
  Get-FileHash 'D:\DSR MOD\components\rmm-bridge\DSRRandomizer.RmmBridgeHost.exe' -Algorithm SHA256
  ```

  Expected: no unexplained worktree changes and hashes equal the publish manifest.

- [ ] **Step 2: Re-run focused safety verification**

  ```powershell
  ctest --preset windows-msvc-release -R "RmmBridge|SaveHookIntegrationTests" --output-on-failure
  dotnet test tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj -c Release
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot 'D:\DSR MOD' -VerifyOnly
  ```

  Expected: all commands exit zero without starting `DarkSoulsRemastered.exe`.

- [ ] **Step 3: Provide the manual smoke-test boundary**

  Tell the user to put Steam in Offline Mode, close any running game/bridge-host process, press `Launch DS1` once, exit normally, and report whether the title screen and character load succeed. Do not claim real-game success until that smoke test is observed; the completed automated claim is limited to build, tests, deployment, configuration, and save-isolation fixtures.

# Overhaul GameParam Redirect Implementation Plan

**Closure (2026-09-01):** The PARAMDEF-aware merge, atomic publication, and exact read redirect are implemented. The original direct Enemy Randomizer `Launch DS1` entrypoint was superseded by the committed launcher-owned `Launch modded copy` flow, which installs the verified project bridge and creates the bridged Mod Engine configuration. This document remains the implementation record; recipient instructions are in `INSTALL_KO.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the launcher-owned integrated Randomizer launch generate and load one verified GameParam that preserves Steam Overhaul rows while applying the current item- and enemy-randomizer row changes.

**Architecture:** The managed RMM bridge host performs a PARAMDEF-aware three-way BND3/PARAM merge before signaling readiness and atomically publishes the result below `<external-root>`. The existing native save-hook owner receives an optional exact GameParam source/target pair and redirects only read-compatible `./overhaul/GameParam.parambnd.dcx` opens and matching attribute probes to the generated file. Bootstrap remains fail-closed and Steam stays read-only.

**Tech Stack:** .NET 8, C# 12, xUnit, SoulsFormatsNEXT pinned source, C++20, Win32, MinHook, CMake 3.28+, PowerShell 7

**Spec:** `docs/superpowers/specs/2026-08-30-overhaul-gameparam-redirect-design.md`

## Global Constraints

- Never write to the Steam installation, normal/Overhaul save roots, or the enemy-randomizer directory.
- All generated/staging/log output must remain under the recipient-selected `<external-root>`.
- Preserve dedicated `.rmm` save isolation. Enemy Randomizer only prepares the copied runtime; the user then closes it, returns to DSR for MOD, and uses `Launch modded copy` rather than Enemy Randomizer's `Launch DS1`.
- Use the active Steam Overhaul binder as a read-only merge target; use the enemy-randomizer bundled vanilla binder as the base and its final binder as the randomizer side.
- Resolve exactly one `DS1EnemyRandomizer` directory below the active runtime `Mods`; ambiguity fails closed.
- Perform functional three-way merge by binder entry and PARAM row ID. Randomizer row additions/changes/deletions win; unchanged randomizer rows preserve Overhaul state.
- Preserve target PARAM headers and binder metadata. Compare payloads exactly, ignore nonfunctional row names, and fail closed on duplicate IDs in a changed PARAM.
- Use the randomizer's existing `dist1\Defs`; do not copy or redistribute its definitions.
- Pin SoulsFormatsNEXT at commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`, reference its source project, and preserve GPL-3.0/source notices.
- The native redirect accepts only the exact relative or configured absolute Overhaul GameParam path and only non-mutating `OPEN_EXISTING` access.
- Reuse the existing `InstallSaveHooks` owner; do not add a second CreateFile hook system.
- Missing, malformed, aliased, swapped, or incompatible sources/target fail closed without fallback.
- Existing user changes in this dirty worktree are authoritative and must be preserved.

## File map

- `.gitmodules`, `third_party/SoulsFormatsNEXT/` — pinned source dependency.
- `src/DSRRandomizer.RmmBridgeHost/GameParam/` — source discovery, exact row comparison, three-way merge, atomic publication, semantic verification, and manifest/log records.
- `src/DSRRandomizer.RmmBridgeHost/BridgeSessionCoordinator.cs` — generation-before-ready orchestration.
- `tests/DSRRandomizer.RmmBridgeHost.Tests/GameParam/` — merge and generation tests.
- `native/runtime/save/SaveHooks.{h,cpp}` — single-owner exact read redirect and attribute consistency.
- `native/runtime/bridge/RmmBridgeConfiguration.{h,cpp}` and `RmmBridgeBootstrap.cpp` — path derivation and configuration transport.
- `native/tests/RmmBridgeConfigurationTests.cpp`, `RmmBridgeBootstrapTests.cpp`, `SaveHookIntegrationTests.cpp` — native policy and integration coverage.
- `scripts/publish-rmm-bridge.ps1` — dependency/build/deploy/verify and manifest checks.
- `docs/enemy-randomizer-rmm-bridge.md`, `THIRD_PARTY_NOTICES.md` — operations and licensing.

---

### Task 1: Pin the parser and implement the tested merge core

**Files:**
- Modify: `.gitmodules`
- Add submodule: `third_party/SoulsFormatsNEXT` at `55b08a3c02a03777cf19958d8f6aa18d7af59da1`
- Modify: `src/DSRRandomizer.RmmBridgeHost/DSRRandomizer.RmmBridgeHost.csproj`
- Create: `src/DSRRandomizer.RmmBridgeHost/GameParam/GameParamMergeModel.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/GameParam/GameParamThreeWayMerger.cs`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/GameParam/GameParamThreeWayMergerTests.cs`
- Modify: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes: three `BND3` objects and a validated set of `PARAMDEF` objects.
- Produces: `GameParamMergeResult Merge(GameParamMergeInputs inputs)` containing output bytes and counts for changed entries, added/changed/deleted rows, preserved target rows, and randomizer-wins overlaps.

- [ ] **Step 1: Add failing synthetic three-way tests**

  Build tiny BND3/PARAM fixtures with one PARAMDEF and assert: base-equal random rows preserve target edits/additions/deletions; randomizer additions/changes/deletions apply; concurrent same-row edits use randomizer payload; names do not cause functional changes; added rows have deterministic ID ordering; duplicate IDs in a changed PARAM fail closed; incompatible param type/row width fails; an unchanged binder entry retains the exact target `Bytes` object content.

- [ ] **Step 2: Pin and reference SoulsFormatsNEXT**

  Add the licensed source as a Git submodule pinned to the exact commit and add a `ProjectReference` to `third_party/SoulsFormatsNEXT/SoulsFormats/SoulsFormats.csproj` selecting `netstandard2.1`. Record the commit, repository URL, GPL-3.0 license, and its runtime package dependencies in `THIRD_PARTY_NOTICES.md`.

- [ ] **Step 3: Confirm red then implement the merge**

  Resolve definitions by exact `ParamType` plus `GetRowSize() == DetectedSize`; do not require header data-version equality. Apply the same compatible definition to base/random/target. Compare cell values bit-exactly (`byte[]` by sequence, `float`/`double` by bit representation, strings ordinal). Clone rows with new `PARAM.Row(id, name, def)` and copied cell values, retaining the target name for replacements when present. Preserve target container/header fields, mutate only changed target PARAMs, and write with `targetBnd.Write()`.

- [ ] **Step 4: Add semantic output verification**

  Reopen output bytes, reapply definitions, and independently assert every union row obeys the three-way rule. Verify binder entry uniqueness, expected DCX/BND3 format, 41-entry real layout compatibility, and unchanged target entries at logical byte level.

- [ ] **Step 5: Run focused tests**

  ```powershell
  dotnet test tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj -c Debug --filter "FullyQualifiedName~GameParamThreeWayMergerTests"
  ```

  Expected: all synthetic merge tests pass.

---

### Task 2: Resolve real inputs and publish atomically before readiness

**Files:**
- Create: `src/DSRRandomizer.RmmBridgeHost/GameParam/GameParamSourceResolver.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/GameParam/GameParamPublisher.cs`
- Create: `src/DSRRandomizer.RmmBridgeHost/GameParam/IGameParamPublisher.cs`
- Modify: `src/DSRRandomizer.RmmBridgeHost/BridgeSessionCoordinator.cs`
- Modify: `src/DSRRandomizer.RmmBridgeHost/Program.cs`
- Modify: `src/DSRRandomizer.RmmBridgeHost/BridgeHostFailureLog.cs`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/GameParam/GameParamSourceResolverTests.cs`
- Create: `tests/DSRRandomizer.RmmBridgeHost.Tests/GameParam/GameParamPublisherTests.cs`
- Modify: `tests/DSRRandomizer.RmmBridgeHost.Tests/BridgeSessionCoordinatorTests.cs`

**Interfaces:**
- Consumes: canonical external root, active runtime ID, verified source-installation root, and exactly one enemy-randomizer directory.
- Produces: verified `content/overhaul/GameParam.parambnd.dcx` plus `gameparam-merge-manifest.json`, before `SignalReady`.

- [ ] **Step 1: Add failing path and orchestration tests**

  Cover zero/multiple randomizer folders, traversal, reparse/hard-link rejection, missing base/random/defs/Overhaul, output escape, and source changes. Assert publisher runs after live binding validation but before save `PrepareAsync`/`BeginSessionAsync` and readiness; publisher failure returns a distinct exit code without signaling readiness or starting a save session.

- [ ] **Step 2: Implement strict source resolution**

  Resolve the active runtime from the already validated runtime ID, enumerate bounded direct candidates below `runtimeRoot\Mods`, and accept exactly one directory whose leaf is `DS1EnemyRandomizer` and which contains the four required input locations. Canonicalize existing inputs, reject reparses/multiple links, and require containment under the expected runtime or verified Steam root. Resolve the output and manifest under `externalRoot\components\rmm-bridge\content\overhaul`.

- [ ] **Step 3: Implement hash-aware atomic publication**

  Hash base/random/Overhaul/all used definition files. Reuse an existing output only when its manifest schema, source hashes, dependency commit, and output hash all match and semantic reopen succeeds. Otherwise merge to a GUID staging file in the output directory, flush, reopen, semantically verify, hash, and atomically replace the output. Clean only the exact staging file on failure.

- [ ] **Step 4: Integrate before readiness**

  Construct the publisher in `Program.cs`. In `BridgeSessionCoordinator`, call it immediately after `ValidateBinding` and before any dedicated-save session mutation. Log source paths/hashes, merge counts, output hash, cache hit/miss, and failures under `<external-root>\logs`; do not log save contents.

- [ ] **Step 5: Run host tests and a read-only real-input generation test**

  ```powershell
  dotnet test tests/DSRRandomizer.RmmBridgeHost.Tests/DSRRandomizer.RmmBridgeHost.Tests.csproj -c Debug
  ```

  Then run a dedicated test command whose inputs are the three current real binders and whose output is a unique test staging directory under `<external-root>\staging`; assert all eight changed PARAMs are applied, the 20 Overhaul-only added rows remain, and Steam/input hashes are unchanged. Remove only that validated staging directory afterward.

---

### Task 3: Transport strict GameParam paths through bridge configuration

**Files:**
- Modify: `native/runtime/bridge/RmmBridgeConfiguration.h`
- Modify: `native/runtime/bridge/RmmBridgeConfiguration.cpp`
- Modify: `native/runtime/bridge/RmmBridgeBootstrap.cpp`
- Modify: `native/runtime/save/SaveHooks.h`
- Modify: `native/tests/RmmBridgeConfigurationTests.cpp`
- Modify: `native/tests/RmmBridgeBootstrapTests.cpp`

**Interfaces:**
- Adds trailing optional `overhaulGameParamSource` and `overhaulGameParamTarget` fields to `BridgeConfiguration` and `SaveHookConfiguration`.
- Existing normal guarded-launch aggregate initializers keep both empty; the RMM bridge sets both.

- [ ] **Step 1: Add failing configuration/bootstrap tests**

  Assert source derives lexically from the live process-image parent as `overhaul\GameParam.parambnd.dcx`; target derives from the bridge-owned external root. Cover the Steam live-image/hard-linked-runtime case. Assert the single hook install receives both exact paths and host failure prevents installation.

- [ ] **Step 2: Implement derivation and transport**

  Add the fields at the end of both configuration structs, derive without requiring the not-yet-generated target to exist, and pass them through `BootstrapRmmBridge` after host readiness.

- [ ] **Step 3: Run native focused tests**

  ```powershell
  cmake --build --preset windows-x64-debug --target RmmBridgeConfigurationTests RmmBridgeBootstrapTests
  ctest --preset windows-x64-debug -R "RmmBridgeConfigurationTests|RmmBridgeBootstrapTests" --output-on-failure
  ```

---

### Task 4: Add the single-owner exact read redirect

**Files:**
- Modify: `native/runtime/save/SaveHooks.h`
- Modify: `native/runtime/save/SaveHooks.cpp`
- Modify: `native/tests/SaveHookIntegrationTests.cpp`
- Modify: `native/runtime/bridge/WindowsBridgePlatform.cpp`

**Interfaces:**
- Produces exact GameParam matching, read-only eligibility, pinned-target open/attribute operations, and a separate `CurrentGameParamRedirectCount()` diagnostic.

- [ ] **Step 1: Add failing path/access matrix tests**

  Cover relative `overhaul\...`, observed `.\overhaul\...`, slash/case variants, and exact absolute source for `CreateFileW/A`, `GetFileAttributesW/A`, and `GetFileAttributesExW/A`. Negative paths include extra components, wrong root, backup suffix, ADS, traversal, repeated separators, short names, UNC, and device namespaces. Access tests allow only non-mutating `OPEN_EXISTING` reads and deny write/delete/create/truncate/unknown access without touching source or target.

- [ ] **Step 2: Validate the optional redirect configuration after host readiness**

  Require both-or-neither paths, `protectFileIo`, canonical absolute source/target with exact structural suffixes, distinct locations, and private regular non-reparse single-link source/target files. Capture stable identities. Empty fields preserve existing launcher behavior.

- [ ] **Step 3: Implement exact matching and pinned target use**

  Match relative paths by a dedicated component parser and absolute paths by canonical lexical equality. Handle an exact but write-like match as access denied. For reads and attributes, revalidate identities, resolve/pin the target leaf, and use the existing reopen path; any swap/missing/alias condition denies without source fallback.

- [ ] **Step 4: Add ANSI attribute hooks to the existing hook set**

  Route `GetFileAttributesA` and `GetFileAttributesExA` through the W logic. Increase hook arrays/rollback expectations from 14 to 16. Keep one atomic `InstallSaveHooks` call and add a separate successful redirect counter.

- [ ] **Step 5: Run hook regressions**

  ```powershell
  cmake --build --preset windows-x64-debug --target SavePathPolicyTests SaveHookIntegrationTests RmmBridgeBootstrapTests
  ctest --preset windows-x64-debug -R "SavePathPolicyTests|SaveHookIntegrationTests|RmmBridgeBootstrapTests" --output-on-failure
  ```

---

### Task 5: Publish, deploy, and verify the current runtime

**Files:**
- Modify: `scripts/publish-rmm-bridge.ps1`
- Modify: `docs/enemy-randomizer-rmm-bridge.md`
- Local generated output: `<external-root>\components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx`
- Local manifest/logs under `<external-root>`

**Interfaces:**
- Consumes: Release bridge/host, pinned SoulsFormats dependency, active runtime inputs, and current bridge TOML.
- Produces: verified deployed artifacts and one generated GameParam without modifying Steam or save inputs.

- [ ] **Step 1: Extend publish verification tests/checks**

  `-VerifyOnly` must require the deployed dependency/artifact hashes, merge manifest schema and source hashes, output hash, semantic reopen, exact current TOML bridge entry, unchanged `.rmm` hash/length, and no running game/host during mutation.

- [ ] **Step 2: Build and deploy atomically**

  Initialize/update the pinned submodule, run managed/native Release builds and tests, stage the host plus dependency outputs under `<external-root>\staging`, hash them, and atomically replace only bridge component files. Do not rewrite the enemy-randomizer TOML unless its exact bridge entry is absent; preserve its existing debug/heap/loose-file settings.

- [ ] **Step 3: Generate and inspect the real merged binder without launching the game**

  Invoke the host's generation-only path or a tested publisher CLI to create the final output. Verify the changed-PARAM set, merged row counts, source/output hashes, 41 binder entries, correct DCX type, and unchanged Steam/RMM hashes.

- [ ] **Step 4: Run full automated verification**

  ```powershell
  dotnet test DSR-Randomizer.sln -c Release
  cmake --preset windows-x64-release
  cmake --build --preset windows-x64-release
  ctest --preset windows-x64-release --output-on-failure
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot '<external-root>'
  & .\scripts\publish-rmm-bridge.ps1 -ExternalRoot '<external-root>' -VerifyOnly
  git diff --check
  ```

- [ ] **Step 5: Perform one controlled enemy-randomizer smoke launch**

  Temporarily enable bridge diagnostics, use DSR for MOD's `Launch modded copy` once, confirm the game and host remain live through title/character load, then exit normally. Verify logs show the source hashes, output hash, and at least one exact GameParam redirect. Confirm the RMM metadata completes normally, the RMM remains fixed length, and sampled Steam files are unchanged. Restore diagnostics to `false` afterward.

---

### Task 6: Final review and handoff

**Files:**
- Verify only: all implementation, tests, deployment artifacts, manifests, logs, Steam samples, and RMM metadata.

- [ ] **Step 1: Run final whole-branch review**

  Review the complete diff against the design spec, with special attention to source/target containment, three-way semantics, alias races, exact access filtering, atomic publication, dependency licensing, and preservation of prior dirty-worktree changes.

- [ ] **Step 2: Re-run completion evidence**

  Re-run the focused managed/native Release suites, publish `-VerifyOnly`, dependency/output hash checks, Steam/RMM before/after comparisons, and `git diff --check` immediately before reporting completion.

- [ ] **Step 3: Handoff**

  Report the deployed output path and hashes, merge counts, redirect evidence, tests, any explicit rulings, rollback command/path, and the exact files changed. Do not claim item/enemy/Overhaul co-loading unless the real smoke evidence is present.

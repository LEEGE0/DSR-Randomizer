# DSR for MOD Redistributable Release Design

**Date:** 2026-08-31
**Status:** Approved under the user's delegated approval authority
**Source handoff:** `HANDOFF_DISTRIBUTION_2026-08-31.md`

## Goal

Produce a redistributable Windows x64 ZIP that lets another Steam owner set up DSR for MOD without receiving any Dark Souls Remastered game file, personal save, Steam ID, log, generated seed, spoiler output, or third-party randomizer executable.

## Chosen Distribution Model

The release bundles only project-owned or already-compliance-reviewed artifacts:

- `DSRForMod.Launcher.exe`
- the native offline/save guard and its SHA-256 sidecar
- the pinned compatibility profile
- `DSRRandomizer.RmmBridge.dll`
- the self-contained `DSRRandomizer.RmmBridgeHost.exe`
- a bridge deployment manifest
- project license, notices, changelog, English overview, and Korean installation guide

Recipients must own Dark Souls Remastered on Steam and obtain the Item Randomizer and Enemy Randomizer from their official distribution pages. The Enemy Randomizer download remains intact and supplies its compatible Mod Engine fork and `DS1HeapPatch.dll`.

The release must not download, mirror, extract, or redistribute those third-party programs. This is required because the Enemy Randomizer explicitly forbids re-uploading, the Item Randomizer repository has no explicit redistribution grant, and the exact compatible Mod Engine/heap-patch binaries are distributed as part of the Enemy Randomizer package.

## Exact Package Layout

The package allowlist is:

```text
DSRForMod.Launcher.exe
README.md
INSTALL_KO.md
LICENSE
CHANGELOG.md
THIRD_PARTY_NOTICES.md
config/compatibility-profiles.json
native/DSRRandomizer.Runtime.dll
native/DSRRandomizer.Runtime.dll.sha256
components/rmm-bridge/DSRRandomizer.RmmBridge.dll
components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe
components/rmm-bridge/deployment-manifest.json
```

No PDB is included. No additional path is accepted by package validation.

The bridge manifest uses UTF-8 without a byte-order mark and this schema:

```json
{
  "schemaVersion": 1,
  "configuration": "Release",
  "bridgeSha256": "<64 lowercase hex characters>",
  "hostSha256": "<64 lowercase hex characters>"
}
```

## Build-Pinned Artifact Identities

The launcher build embeds SHA-256 identities for four immutable inputs:

1. `native/DSRRandomizer.Runtime.dll`
2. `config/compatibility-profiles.json`
3. `components/rmm-bridge/DSRRandomizer.RmmBridge.dll`
4. `components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe`

The launcher project fails to build if any pinned input is missing. Package validation opens each artifact through the existing lease mechanism and compares its bytes with the embedded identity. It also parses the bridge manifest strictly and requires its hashes to match the same embedded bridge and host identities.

## Bridge Installation Contract

A focused launcher service, `RmmBridgeBundleInstaller`, owns bridge deployment. It consumes the package directory, selected external root, and embedded identities. It produces an installed and re-verified bridge pair at:

```text
<external-root>/components/rmm-bridge/DSRRandomizer.RmmBridge.dll
<external-root>/components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe
<external-root>/components/rmm-bridge/deployment-manifest.json
```

The operation has these properties:

- It requires no runtime pointer, Steam ID, save selection, `.rmm`, metadata, randomizer installation, or generated seed.
- It rejects source or destination reparse points and rejects any path that escapes the selected external root.
- It opens and hashes the packaged DLL, host, and manifest before writing.
- It writes temporary sibling files, flushes them, then replaces the DLL and host before writing the manifest last as the coherence marker.
- It preserves `components/rmm-bridge/content`, including generated Overhaul merge content.
- It reopens and rehashes the installed DLL, host, and manifest before reporting success.
- If interrupted, a later invocation repairs the pair. A missing or stale manifest prevents launch, so a mixed pair fails closed.
- If the installed pair already matches, it performs no replacement.

The launcher invokes this operation immediately before creating the bridged Mod Engine configuration. Missing, tampered, or un-installable bridge artifacts stop launch before Mod Engine starts.

Stable error codes are:

- `RMM_BRIDGE_BUNDLE_INVALID` for a missing or hash-invalid packaged source
- `RMM_BRIDGE_INSTALL_FAILED` for a safe write/replace failure
- `RMM_BRIDGE_INSTALL_TAMPERED` when post-install verification fails

## Randomizer Integration Boundary

The existing randomizer discovery remains responsible for finding a user-installed Item Randomizer, Enemy Randomizer, Mod Engine launcher/library/configuration, and `DS1HeapPatch.dll` beneath the copied runtime. It must not acquire those files.

After bridge installation succeeds, the launcher creates `staging/diagnostics/config-randomizer-bridged.toml` using:

- the installed bridge DLL as the first external DLL
- the user-supplied `DS1HeapPatch.dll`
- the user-supplied randomizer mod directory
- the installed bridge component directory as the bridge mod directory

The launcher still validates that the third-party paths exist and conform to the existing expected layout before starting Mod Engine.

## Developer Bridge Publisher Recovery Rule

`scripts/publish-rmm-bridge.ps1` remains a developer deployment tool, not the end-user installer. Its pre-deployment save check must match runtime recovery semantics:

- `cleanExit` must be a JSON boolean.
- Hash calculation failure always blocks deployment.
- A `.rmm` hash mismatch blocks deployment only when `cleanExit` is `true`.
- A `.rmm` hash mismatch with `cleanExit=false` is allowed so the managed host can recover the interrupted session.

The script must not weaken any other path, runtime, or process-safety check.

## Native Integration Fixture

Production pinned executable verification remains unchanged. The synthetic `RmmBridgeIntegrationTests` executable cannot satisfy the real game's exact size, PE identity, and SHA-256, so the test must use a separate test-only bridge/profile target.

The test-only target is produced under a distinct filename and is never copied to the release package. It may resolve fixture callsites through a fixture-only exported contract or compile-time test profile, but the production `DSRRandomizer.RmmBridge.dll` must not accept environment variables, command-line switches, external profile files, or other bypasses for pinned identity verification.

The integration test must still prove:

- bridge host readiness
- callsite preparation and installation
- redirected write to `DRAKS0005.rmm`
- denial of access to the normal `.sl2`
- no virtual normal save creation

## Package Validation and ZIP Construction

Release construction is one orchestration path that:

1. builds all native Release targets;
2. publishes the self-contained bridge host;
3. publishes the self-contained launcher with pinned guard/profile/bridge/host paths;
4. stages exactly the allowlisted files;
5. writes the bridge manifest and native guard sidecar;
6. runs `DSRForMod.Launcher.exe --validate-package` against staging;
7. creates a deterministic ZIP with sorted entries and fixed timestamps;
8. validates ZIP entry names for duplicates, rooted paths, `..`, and unexpected directories;
9. extracts the ZIP into a fresh temporary directory and runs the same launcher validator again;
10. writes `<zip>.sha256` using lowercase SHA-256 and the ZIP filename.

The dependency manifest used to assemble .NET notices must come from the supplied launcher publish directory or its exact publish invocation, never the newest unrelated `obj/Release` file.

Before delivery, an additional privacy scan checks the extracted package for the known private-root, private-worktree, and Steam-ID sentinels supplied outside committed documentation. The exact allowlist already excludes saves, logs, profiles, staging, seeds, spoilers, game files, and randomizer executables.

## Korean Installation Guide

`INSTALL_KO.md` explains:

1. install/verify a legitimate Steam Dark Souls Remastered copy;
2. download Item Randomizer and Enemy Randomizer only from their official pages;
3. keep the Enemy Randomizer directory intact so its matching Mod Engine and heap patch remain together;
4. choose a new external root with sufficient disk space;
5. initialize the copied runtime through the launcher;
6. place the user-supplied randomizer directories in the documented copied-runtime layout;
7. run the randomizers and then launch the modded copy;
8. use Steam Offline Mode as a user-managed prerequisite;
9. never copy another person's `.sl2`, `.rmm`, Steam ID folder, seed, or spoiler file.

The guide links official sources and states that those projects are not included in the ZIP.

## Verification Gate

A release is deliverable only after fresh evidence for all of the following:

- `dotnet test DSR-Randomizer.sln -c Release --no-restore` passes all managed tests.
- `pwsh -NoProfile -File scripts/build-native.ps1 -Configuration Release -Test` passes all 15 native tests.
- the clean-root bridge installer tests pass without using a pre-existing private runtime or an existing save/profile/runtime pointer.
- the published package validator accepts the staged directory and the freshly extracted ZIP.
- ZIP inspection confirms the exact allowlist and no local game/personal artifacts.
- the SHA-256 sidecar matches a fresh hash of the final ZIP.

If any verification is red, the release must be reported as incomplete; no “all tests pass” claim is allowed.

## Release Output

The first completed redistributable revision uses version `0.1.0-alpha.2` and produces:

```text
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip.sha256
```

The worktree's pre-existing modifications remain part of the feature branch. No reset, checkout-based rollback, bulk deletion, or copying from a private local runtime is permitted.

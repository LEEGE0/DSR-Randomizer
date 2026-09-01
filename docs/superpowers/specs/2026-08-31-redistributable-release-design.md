# DSR for MOD Redistributable Release Design

**Date:** 2026-08-31
**Status:** Approved under the user's delegated approval authority
**Source handoff:** `HANDOFF_DISTRIBUTION_2026-08-31.md`

## Goal

Produce a redistributable Windows x64 ZIP that lets another Steam owner set up DSR for MOD without receiving any Dark Souls Remastered game file, personal save, Steam ID, log, generated seed, spoiler output, or third-party randomizer executable.

## Chosen Distribution Model

The binary release bundles only project-owned or already-compliance-reviewed artifacts:

- `DSRForMod.Launcher.exe`
- the native offline/save guard and its SHA-256 sidecar
- the pinned compatibility profile
- `DSRRandomizer.RmmBridge.dll`
- the self-contained `DSRRandomizer.RmmBridgeHost.exe`
- a bridge deployment manifest
- project license, notices, changelog, English overview, and Korean installation guide

Recipients must own Dark Souls Remastered on Steam and obtain the Item Randomizer and Enemy Randomizer from their official distribution pages. The Enemy Randomizer download remains intact and supplies its compatible Mod Engine fork and `DS1HeapPatch.dll`.

The release must not download, mirror, extract, or redistribute those third-party programs. This is required because the Enemy Randomizer explicitly forbids re-uploading, the Item Randomizer repository has no explicit redistribution grant, and the exact compatible Mod Engine/heap-patch binaries are distributed as part of the Enemy Randomizer package.

The binary bridge host compiles a project-owned subset of SoulsFormatsNEXT commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`. Modified by DSR for MOD on 2026-09-01 to omit TPF/DrSwizzler support for the bridge-host build. The subset also avoids the unused BouncyCastle runtime, retains BND3/PARAM/DCX_DFLT and required Zstd support, and leaves the pinned upstream checkout clean. Release tests parse the .NET v6 single-file bundle manifest and embedded deps JSON to prove both excluded dependencies are absent.

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

## Authoritative Outer Archive and Corresponding Source

The exact 12-path binary layout remains unchanged as an inner component. The only authoritative file intended for delivery is:

```text
DSR-for-MOD-v0.1.0-alpha.2-redistributable.zip
```

That deterministic outer ZIP has exactly three root entries, in ordinal order with fixed timestamps: `DSR-for-MOD-v0.1.0-alpha.2-source.zip`, `DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip`, and `SHA256SUMS.txt`. The hash manifest has exactly two LF-terminated lines in the same order, each containing a lowercase SHA-256, two spaces, and the exact inner filename. No external sidecar is created for the outer archive.

The source archive is generated from committed `HEAD`, then overlays the actual contents of all three pinned upstream commits rather than their gitlinks alone:

- SoulsFormatsNEXT `55b08a3c02a03777cf19958d8f6aa18d7af59da1`, including its complete source and GPL license;
- ZstdNet `c90152918f633e945f163652e6368001556784e7`, including its managed source, project/build inputs, and BSD license;
- Zstandard `b706286adbba780006a47ef92df0ad7a785666b6`, including its native library source, build inputs, and BSD license.

It uses one versioned root prefix, ordinally sorted unique entries, and a fixed 1980 timestamp. A generated `SOURCE_REVISIONS.json` has exactly `schemaVersion`, `mainRevision`, and `submodules`; the submodule object names all three paths and their exact commits. The archive contains the main solution, subset project and modification notice, build/release scripts, project license files, and those three complete pinned trees. It excludes `.git`, `bin`, `obj`, `artifacts`, `.superpowers`, private/generated working data, traversal, rooted paths, aliases, and duplicates. On Windows, recipients should extract it near a drive root or system temporary root to avoid legacy MSBuild path-length behavior; the included README provides exact host restore/build commands.

The complete authoritative outer archive must be conveyed. It physically keeps the exact inner binary/source pair and their hash manifest together, satisfying the chosen same-place source distribution. A tracked package document must not embed the source ZIP's own hash because that would make the archive identity self-referential.

## Immutable Release Source State

Before any official binary build, the main repository must resolve to a committed `HEAD`; all tracked files and all nonignored untracked files must be clean. Ignored generated outputs such as `artifacts`, `bin`, and `obj` are permitted. Every recursive submodule must be initialized, at the exact gitlink revision, and clean, and the three release-contract revisions above must match exactly. Errors identify whether the main tree or a named submodule violates the invariant.

The same invariant is checked after binary staging and again inside the source builder immediately before it archives committed objects. The binary and source builders may create private sidecars inside the verified work directory, but those are checked strictly and never published. After the final package, privacy, source-tree, and extracted-source build checks, the builder captures both exact inner ZIP hashes and constructs the deterministic outer ZIP from stable leased inner inputs. It validates the exact three-entry order/timestamps, strict hash manifest, and the exact inner bytes before publication.

Publication acquires a canonical, regular, single-link output-root lock. The persistent lock file is empty, ignored, and outside archives/upload globs; a cooperative concurrent publisher that encounters the held lock receives stable `PUBLICATION_IN_PROGRESS` without changing the canonical archive. The publisher leases the exact gated outer input with write/delete replacement denied and verifies its expected SHA-256 and semantics. It creates the same-filesystem pending file with READ/DELETE access (plus write while creating) and denies write/delete sharing. The exact handle stays open through durable copy and `Flush(true)`. `BeforeHandleRename` runs before a final same-handle check of regular/non-reparse/single-link identity, expected SHA-256, and exact outer semantics. Only then does `SetFileInformationByHandle(FileRenameInfo)` atomically rename that opened object over the canonical name; successful native return updates an immutable committed marker and syntactic target immediately, with no path-resolution call in the transition. This is the sole commit point, and the caller transfers the handle into committed ownership before any fallible final-path/content validation. Any earlier failure leaves the old canonical byte-exact or preserves first-publish absence. There is no prior-file backup, rollback candidate, failed-canonical rename, transaction directory, journal, or multi-file phase. Before the final success observation, the publisher explicitly disposes and clears the staged-source lease, pending/non-final wrappers, publication lock, and output-root lease; any error is a committed-new failure, never success. Only the exact canonical handle then remains live with write/delete replacement denied. SHA-256 and exact outer validation run first; exact final-path resolution runs next; and a fresh file-information query makes regular/non-reparse attributes plus `NumberOfLinks == 1` the final observation. The immediate next operation disposes that canonical handle, followed only by trivial ownership clearing and return. Delayed hard links created at each former resource-unwind seam are therefore caught by the final query and schedule handle-based canonical removal fail-closed. A cooperative publisher entering after root-lock disposal still cannot replace the live write/delete-denying canonical and must retry after failure. The locally enforceable guarantee ends at immediate final-handle disposal; same-user namespace or content mutations after disposal are outside the contract. Legacy cleanup takes only a validated version, derives the exact four obsolete loose artifact names internally, and preserves reparse or multi-link files. Tests cover deterministic construction, first publication, replacement, actual/injected native-rename failure, injected post-rename final-path resolution failure, pending replacement, pre/post-rename hard-link insertion, delayed hard links during the long scan and at all former unwind seams, explicit non-final disposal ordering, final-handle release before return, post-commit replacement/failure, synchronized publishers and retry, exact exports, and restricted safe cleanup of the four obsolete loose artifact paths.

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
7. creates the deterministic 12-path inner binary ZIP with sorted entries and fixed timestamps;
8. validates binary ZIP entry names for duplicates, rooted paths, `..`, and unexpected directories;
9. extracts the binary ZIP into a fresh temporary directory and runs the same launcher validator again;
10. creates and validates the deterministic corresponding-source ZIP;
11. constructs the exact three-entry outer redistributable and strict two-line `SHA256SUMS.txt` from the gated inner bytes;
12. publishes only the one outer file atomically and removes only safe exact legacy loose artifact paths.

The dependency manifest used to assemble .NET notices must come from the supplied launcher publish directory or its exact publish invocation, never the newest unrelated `obj/Release` file.

Before delivery, an additional privacy scan checks every binary/source archive entry for reviewed local-profile and account sentinels. It covers plain Windows paths, JSON-escaped backslashes, forward-slash paths, file URIs, UTF-8, UTF-16LE, and UTF-16BE, while constructing the reviewed values from noncontiguous fragments in tracked scanner source. Project-owned fixtures use neutral synthetic identities; pinned upstream trees are audited read-only. The exact binary allowlist already excludes saves, logs, profiles, staging, seeds, spoilers, game files, and randomizer executables.

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
- package identity leases remain reparse-safe and validate regular files whose absolute staging paths exceed the legacy Windows 260-character boundary.
- ZIP inspection confirms the exact allowlist and no local game/personal artifacts.
- `SHA256SUMS.txt` exactly matches fresh hashes of both inner ZIPs and the separately reported outer SHA-256 matches the final outer bytes.
- the official host's parsed .NET v6 bundle manifest and embedded deps JSON contain neither DrSwizzler nor BouncyCastle, while every retained non-runtime dependency has a complete shipped notice.
- the source archive has deterministic safe entries, matches the committed project plus every file in the exact pinned SoulsFormatsNEXT, ZstdNet, and Zstandard trees, excludes repository/build/private state, and its hash matches the outer manifest.
- the outer archive has exactly the three ordered fixed-timestamp entries, no duplicates/aliases/traversal, no external sidecar, and its exact inner bytes retain every binary/source gate.
- the extracted source can restore and build the bridge-host project.

If any verification is red, the release must be reported as incomplete; no “all tests pass” claim is allowed.

## Release Output

The first completed redistributable revision uses version `0.1.0-alpha.2` and produces:

```text
artifacts/DSR-for-MOD-v0.1.0-alpha.2-redistributable.zip
```

The worktree's pre-existing modifications remain part of the feature branch. No reset, checkout-based rollback, bulk deletion, or copying from a private local runtime is permitted.

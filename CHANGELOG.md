# Changelog

All notable changes to DSR Randomizer are documented here.

## [0.1.0-alpha.2] - 2026-09-01

### Added

- Redistributable Windows x64 package with the project-owned native RMM bridge DLL and self-contained bridge host.
- Recipient-controlled bridge installation beneath the selected external root, with strict four-property manifest validation, embedded SHA-256 identities, safe replacement, final re-verification, and matching no-op behavior.
- Korean recipient guide covering official third-party download sources, the copied-runtime layout, Steam Offline Mode, save isolation, validation, rollback, and redistribution exclusions.
- Exact 12-path inner binary allowlist, staged and freshly extracted package validation, and deterministic ZIP construction.
- One authoritative deterministic outer redistributable containing the exact binary ZIP, corresponding-source ZIP, and strict `SHA256SUMS.txt`; the source contains the committed project tree plus the actual pinned SoulsFormatsNEXT, ZstdNet 1.4.5, and Zstandard 1.4.5 contents, with a strict manifest identifying all four revisions.

### Changed

- `Launch modded copy` installs or repairs the project bridge before generating the bridged Mod Engine configuration and retains verified bridge/host leases through startup.
- Item Randomizer, Enemy Randomizer, the Enemy package's compatible Mod Engine fork, and `DS1HeapPatch.dll` remain recipient-supplied and are neither bundled nor installed by this release.
- The native integration fixture now uses a separate test-only callsite profile while production pinned executable verification remains unchanged.
- The release builder uses unique fail-if-exists work, staging, and extraction directories; rejects reparse/alias escapes; cleans only verified descendants; and preserves final ZIP/checksum outputs on successful cleanup.
- The bridge-host Release publish contains no private absolute PDB path after the byte-level privacy gate rejected an earlier local-path disclosure.
- The project-owned SoulsFormats subset omits TPF/DrSwizzler support and the unused BouncyCastle runtime while retaining the BND3/PARAM/DCX merge path; full ZstdNet/libzstd notices and SoulsFormatsNEXT corresponding-source obligations are included.
- The release builder now fails closed unless the main repository is committed and clean and every recursive submodule is initialized, clean, and exactly at its pinned gitlink; it checks the same invariant again after binary staging and before source archiving.
- Reviewed profile/account sentinels are rejected across binary and source entries in plain, JSON-escaped, forward-slash/URI, UTF-8, UTF-16LE, and UTF-16BE forms; project-owned fixtures use neutral synthetic identities.
- The builder now creates and validates both inner ZIPs privately, binds their exact bytes in the outer hash manifest, and publishes only the one leased outer file under a canonical output-root lock. A same-filesystem pending file is durably written and fully revalidated through its exact still-open Windows handle; successful handle-based atomic rename is recorded immediately as the sole commit point. Pre-commit faults leave old bytes or first-publish absence unchanged. There is no backup or rollback namespace after commit: the renamed final handle remains live through SHA-256/outer validation followed by a last regular/non-reparse/single-link/exact-path check at the return boundary. Nonessential post-commit faults are reported as committed-new, and even a hard link added asynchronously during the byte scan is rejected by the final check, removing only the canonical link through that handle rather than accepting alias-mutable output. Concurrent publishers receive `PUBLICATION_IN_PROGRESS`; legacy cleanup derives only the four exact former loose ZIP/sidecar paths from a validated version and preserves unsafe links.
- Package identity validation uses extended Windows paths for its reparse-safe file leases, so deeply nested release staging remains verifiable beyond the legacy 260-character boundary.

### Verification

- 447/447 managed Release tests and 15/15 native Release tests pass.
- Clean-root bridge installation, tamper repair, exact-three-entry outer validation, extracted inner-binary validation, parsed host bundle/dependency inspection, deterministic exact-three-submodule source-archive validation, clean source-state enforcement, strict manifests/hashes, prohibited-content scans, exact-entry inspection, extracted-source host rebuild, and inner/outer checksum recomputation are part of the release gate.

## [0.1.0-alpha.1] - 2026-08-24

### Added

- Windows x64 WPF foundation launcher.
- Read-only verification of a Dark Souls Remastered source installation.
- Explicit stock-file catalog that excludes installed Overhaul and companion files.
- Full independent runtime copies under the user-selected external material root; `%LOCALAPPDATA%\DSR-Randomizer` contains only the external-root pointer.
- Staged copy verification with SHA-256, source snapshots, rollback, and atomic runtime activation.
- Runtime-readiness validation and command-line status output.
- Original-installation before/after immutability proof.
- Single-file packaging, prohibited-content guard, deterministic ZIP, and SHA-256 checksum.
- User-selected external material root and copied mod runtime with folder-based manual mod installation.
- Shared CLI/WPF modded launch path using an exact authenticated, sessionless save-only `0x7` guard handshake.
- Existing dedicated `.rmm` reuse without normal-save access and atomic read-only bootstrap when `.rmm` is absent.
- Deterministic packaging of the project native guard, guard checksum, and pinned compatibility profile.

### Safety boundary

- Steam Offline Mode is required and remains user-managed; the launcher does not block or attest networking.
- Product launch requests only bootstrap, known-folder save redirection, and file-I/O save redirection (`0x7`).
- The original installation, installed Overhaul, and normal `.sl2` remain protected; only the copied mod runtime and dedicated `.rmm` are launched.
- Automatic randomizer generation, mod enable/disable state, load-order management, and rollback are not included.

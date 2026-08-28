# Changelog

All notable changes to DSR Randomizer are documented here.

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

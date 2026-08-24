# Changelog

All notable changes to DSR Randomizer are documented here.

## [0.1.0-alpha.1] - 2026-08-24

### Added

- Windows x64 WPF foundation launcher.
- Read-only verification of a Dark Souls Remastered source installation.
- Explicit stock-file catalog that excludes installed Overhaul and companion files.
- Full independent external runtime copies under `%LOCALAPPDATA%\DSR-Randomizer`.
- Staged copy verification with SHA-256, source snapshots, rollback, and atomic runtime activation.
- Runtime-readiness validation and command-line status output.
- Original-installation before/after immutability proof.
- Single-file packaging, prohibited-content guard, deterministic ZIP, and SHA-256 checksum.

### Safety lock

- Game launch is deliberately unavailable in this release.
- Item and enemy randomization, dedicated-save redirection, auto-equip, and official-online blocking are not included yet.
- Launch remains locked until dedicated-save and official-online protections are implemented and verified.

# DSR for MOD

DSR for MOD creates an isolated Dark Souls Remastered mod runtime beneath a recipient-selected external root. It protects the Steam installation and normal save as inputs and launches only the verified copied game.

`v0.1.0-alpha.2` is a Windows x64 redistributable alpha. Start with the recipient-facing Korean guide in [`INSTALL_KO.md`](INSTALL_KO.md). The approved release design is recorded in [`docs/superpowers/specs/2026-08-31-redistributable-release-design.md`](docs/superpowers/specs/2026-08-31-redistributable-release-design.md).

## Redistribution boundary

The release ZIP contains exactly 12 allowlisted paths: the project launcher; native offline/save guard and checksum; pinned compatibility profile; project-owned RMM bridge DLL and self-contained bridge host; a strict bridge deployment manifest; and the license, notices, changelog, overview, and Korean guide. Package validation rejects any extra or missing path and any mismatched pinned artifact.

The archive does **not** contain, download, or install:

- Dark Souls Remastered executables, assets, or copied game data;
- `.sl2` or `.rmm` saves, Steam IDs, profiles, logs, staging, seeds, or spoilers;
- Item Randomizer or Enemy Randomizer;
- the Enemy Randomizer package's compatible Mod Engine fork; or
- `DS1HeapPatch.dll`.

Recipients must own Dark Souls Remastered and obtain the two randomizers from their official distribution pages: [Item Randomizer releases](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases) and [Enemy Randomizer files](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files). Their files remain recipient-controlled beneath the active copied runtime and are never added to this project's distributable archive.

## Safety and launch model

Runtime construction copies all and only verified stock-catalog entries into `<external-root>\runtimes\runtime-<id>`. Installed `d3d11.dll`, `d3d11_mod.ini`, Overhaul content, companion DLLs, credentials, logs, dumps, and any other unlisted source content are excluded. The original Steam installation and installed Overhaul remain read-only.

The packaged native guard and compatibility profile are checked against hashes embedded in the launcher. For the randomizer path, `Launch modded copy` first installs or repairs only the packaged project-owned bridge and host at `<external-root>\components\rmm-bridge`, validates their strict four-property manifest and hashes, and retains verified leases through Mod Engine startup. It then creates a bridged configuration from the recipient-supplied Enemy Randomizer layout. Missing, altered, or unsafe bridge artifacts fail closed before the recipient-supplied Mod Engine starts.

Steam Offline Mode is a user-managed prerequisite. The launcher does not inspect Steam state, block networking, change Steam settings, modify the firewall, or disable adapters. Use the DSR for MOD launch button after running Item Randomizer first and Enemy Randomizer last; do not use Enemy Randomizer's `Launch DS1` button for this integrated workflow.

The modded copy uses only `<external-root>\saves\<SteamID>\DRAKS0005.rmm`. A valid existing `.rmm` is reused without opening the normal save. If it is absent, launch performs a read-only, verified, atomic bootstrap from the explicitly selected normal `DRAKS0005.sl2`. The normal `.sl2` is never written.

## Commands

```text
DSRForMod.Launcher.exe --set-root <external-root>
DSRForMod.Launcher.exe --verify <game-path>
DSRForMod.Launcher.exe --initialize-runtime <game-path>
DSRForMod.Launcher.exe --prepare-save <SteamID>
DSRForMod.Launcher.exe --launch <SteamID>
DSRForMod.Launcher.exe --status
DSRForMod.Launcher.exe --validate-package <directory>
```

## Release verification

The `0.1.0-alpha.2` release path builds in unique, reparse-safe work directories, cleans only verified work descendants, validates staging and a fresh ZIP extraction, and emits a SHA-256 sidecar. The bridge-host Release publish omits debug records so a local build path is not embedded in the shipped executable. The final gate covers 436 managed tests and 15 native tests. Native integration uses a distinct test-only callsite profile; production pinned executable verification remains strict. Generated archives and checksums remain build artifacts and are not source-controlled.

## License

SPDX-License-Identifier: GPL-3.0-only

Copyright (C) 2026 DSR for MOD contributors.

Dark Souls and Dark Souls Remastered are trademarks of their respective owners. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Binary redistribution must also satisfy the corresponding-source obligations described in the notices.

# DSR Randomizer

DSR Randomizer provides an isolated Dark Souls Remastered mod runtime. Mods are installed or removed manually as folders in the copied runtime; this release does not generate randomizer content or maintain a mod enable/disable database.

The current design is documented in [`docs/superpowers/specs/2026-08-28-simplified-offline-mod-runtime-design.md`](docs/superpowers/specs/2026-08-28-simplified-offline-mod-runtime-design.md).

## Safety promise

The launcher treats the user's original Dark Souls Remastered installation, existing Overhaul installation, and normal `.sl2` save as protected inputs. It builds and modifies only a separate runtime beneath the selected external root.

Runtime construction copies all and only the verified stock catalog entries. Installed `d3d11.dll`, `d3d11_mod.ini`, the `overhaul` directory, `DSRQuickSummonCompanion.dll`, logs, crash dumps, credentials, and other unlisted source content are excluded.

## Alpha status

`v0.1.0-alpha.1` requires Windows x64, a supported Steam Dark Souls Remastered build, and approximately 9 GB of additional free disk space. Material data lives beneath a user-selected external root; `%LOCALAPPDATA%\DSR-Randomizer` stores only the small external-root pointer.

Before launch, place Steam in Offline Mode. The launcher displays this prerequisite but does not inspect Steam state, block networking, change Steam settings, modify the firewall, or disable adapters. It starts only the verified copied executable with the project guard's exact save-only `0x7` one-shot contract. The historical `0x7F` and monitored paths remain experimental and are not used by the product launch command.

The modded copy uses only `<external-root>\saves\<SteamID>\DRAKS0005.rmm`. A valid existing `.rmm` is reused without opening the normal save. If it is absent, launch performs the existing read-only, atomic bootstrap from the explicitly selected normal `DRAKS0005.sl2` before creating the game process. The normal `.sl2` is never written.

Commands:

```text
DSRRandomizer.Launcher.exe --set-root <external-root>
DSRRandomizer.Launcher.exe --verify <game-path>
DSRRandomizer.Launcher.exe --initialize-runtime <game-path>
DSRRandomizer.Launcher.exe --prepare-save <SteamID>
DSRRandomizer.Launcher.exe --launch <SteamID>
DSRRandomizer.Launcher.exe --status
DSRRandomizer.Launcher.exe --validate-package <directory>
```

The graphical launcher exposes only the copied mod runtime launch path; it never launches the source installation or Overhaul. Steam Offline Mode is a user-managed prerequisite, not a network-isolation guarantee supplied by the launcher.

## License

SPDX-License-Identifier: GPL-3.0-only

Copyright (C) 2026 DSR Randomizer contributors.

Dark Souls and Dark Souls Remastered are trademarks of their respective owners. This project does not include or redistribute game executables, assets, saves, credentials, or locally extracted game data. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

# DSR Randomizer

DSR Randomizer is a planned Dark Souls Remastered mod with deterministic item and boss permutations, weighted regular-enemy placement, automatic equipment on pickup, an isolated save, and a dedicated offline launcher.

The project is currently building `v0.1.0-alpha.1`, the external-runtime isolation foundation. The approved architecture is documented in [`docs/superpowers/specs/2026-08-24-dsr-randomizer-design.md`](docs/superpowers/specs/2026-08-24-dsr-randomizer-design.md).

## Safety promise

The mod treats the user's original Dark Souls Remastered installation and existing Overhaul installation as read-only inputs. It builds and modifies only a separate local runtime outside the Steam installation.

The foundation launcher copies only explicitly recognized stock files. Installed `d3d11.dll`, `d3d11_mod.ini`, the `overhaul` directory, `DSRQuickSummonCompanion.dll`, logs, crash dumps, credentials, and other unlisted root content are excluded.

## Alpha status

`v0.1.0-alpha.1` requires Windows x64, a Steam Dark Souls Remastered installation, and approximately 9 GB of additional free disk space. It can verify the selected installation and create a complete independent runtime under `%LOCALAPPDATA%\DSR-Randomizer`.

This alpha deliberately cannot start the game. Dedicated-save redirection and official-online blocking are not implemented yet, so the launch control remains locked. It is an isolation foundation, not a playable randomizer release.

Foundation commands:

```text
DSRRandomizer.Launcher.exe --verify <game-path>
DSRRandomizer.Launcher.exe --initialize-runtime <game-path>
DSRRandomizer.Launcher.exe --status
DSRRandomizer.Launcher.exe --validate-package <directory>
```

The graphical launcher provides installation verification and external-runtime creation without exposing original-game or Overhaul launch buttons.

## License

SPDX-License-Identifier: GPL-3.0-only

Copyright (C) 2026 DSR Randomizer contributors.

Dark Souls and Dark Souls Remastered are trademarks of their respective owners. This project does not include or redistribute game executables, assets, saves, credentials, or locally extracted game data. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

# DSR Randomizer

DSR Randomizer is a planned Dark Souls Remastered mod with deterministic item and boss permutations, weighted regular-enemy placement, automatic equipment on pickup, an isolated save, and a dedicated offline launcher.

The project is currently in the design phase. The approved architecture is documented in [`docs/superpowers/specs/2026-08-24-dsr-randomizer-design.md`](docs/superpowers/specs/2026-08-24-dsr-randomizer-design.md).

## Safety promise

The mod treats the user's original Dark Souls Remastered installation and existing Overhaul installation as read-only inputs. It builds and modifies only a separate local runtime outside the Steam installation.

## License

GPL-3.0-only. Dark Souls and Dark Souls Remastered are trademarks of their respective owners. This project does not include or redistribute game executables, assets, saves, credentials, or locally extracted game data.

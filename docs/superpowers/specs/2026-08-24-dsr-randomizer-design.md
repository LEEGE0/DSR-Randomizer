# DSR Randomizer Design

Date: 2026-08-24  
Status: Approved design

## 1. Purpose

DSR Randomizer is a dedicated-launcher mod for the Steam version of Dark Souls Remastered on Windows. It creates deterministic, progression-safe item and enemy permutations and automatically equips newly acquired gear. The mod runs only from its own launcher, stores all generated game data outside the Steam installation, uses a dedicated save, and blocks official online play.

The existing original-game and Overhaul launch paths remain outside this project's control. Installing, running, updating, or removing DSR Randomizer must not change either of them.

## 2. Goals

- Provide one dedicated launcher for Random Dark Souls.
- Keep the original game installation and existing Overhaul installation unchanged.
- Store the local runtime, generated seed, save, configuration, and logs outside the Steam installation.
- Generate reproducible item, regular-enemy, and boss placements from a visible and shareable seed.
- Preserve item quantities and boss populations through strict permutations. Preserve the number of regular-enemy destination slots while allowing weighted, duplicate-producing regular-enemy draws.
- Place randomized progression items only where the resulting game remains completable.
- Scale randomized regular enemies and the randomized tutorial boss during the first Undead Asylum visit.
- Automatically equip newly acquired weapons, shields, casting tools, armor, and rings according to fixed rules.
- Use one dedicated randomizer save and reset it only after a newly generated seed passes all validation.
- Prevent the random runtime from connecting to official matchmaking.
- Publish source, documentation, builds, tags, and releases in a public GPL-3.0-only GitHub repository.

## 3. Non-goals

- DSR Randomizer will not install files into the Steam game directory.
- It will not rename, swap, patch, restore, or delete original or Overhaul files.
- It will not launch or manage the original-game or Overhaul profiles.
- It will not redistribute the game executable, game assets, saves, credentials, or extracted local catalogs.
- It will not support official online play from the random runtime.
- The first stable release will not provide a custom multiplayer service.
- The first stable release will not randomize friendly NPCs, merchants as characters, or technical helper entities.

## 4. Safety invariant

The original Dark Souls Remastered installation is a read-only source. Every component must enforce this invariant.

The launcher resolves and canonicalizes all paths before performing a write. A write target is permitted only when it is a descendant of the DSR Randomizer local-data root. The Steam installation path and every descendant are explicitly denied. The process aborts before mutation when a path cannot be resolved unambiguously.

The default local-data root is:

```text
%LOCALAPPDATA%\DSR-Randomizer\
```

The source repository is independent of local game data and defaults to:

```text
%USERPROFILE%\Documents\DSR-Randomizer\
```

The runtime is a full local copy, not a hard-linked mirror. This costs approximately 9 GB for the currently installed game but prevents an in-place write from propagating to the original installation.

## 5. System architecture

```text
RandomizerLauncher.exe
├── Launcher.UI
├── Randomizer.Core
├── GameData
└── launches local runtime
    ├── DarkSoulsRemastered.exe (user-local copy; never distributed)
    ├── RandomizerRuntime.dll
    └── generated randomized game data
```

### 5.1 Launcher.UI

The Windows desktop launcher is implemented with C#/.NET 8 and WPF and is published as a self-contained Windows x64 application. Its primary controls are:

- Game-installation selection and read-only verification
- Seed text entry
- New Seed
- Copy Seed
- Paste Seed
- Generate and Validate
- Launch Random Dark Souls
- Current seed, placement hash, runtime version, save state, and offline-protection status

The launcher does not expose original-game or Overhaul launch buttons. Those existing environments remain independent.

### 5.2 Randomizer.Core

The core contains deterministic random-number streams, item placement, enemy placement, progression solving, placement validation, and stable serialization. Item, regular-enemy, and boss randomization use independently derived streams. A change to one category must not silently reshuffle the other categories for the same seed and format version.

Each generated result records:

- User-visible seed
- Randomizer version
- Game-data catalog hash
- Configuration schema version
- Item placement hash
- Regular-enemy placement hash
- Boss placement hash
- Combined placement hash

### 5.3 GameData

GameData reads and writes DSR formats with SoulsFormats-based tooling. It imports only from the user's installed copy and writes only into the external local runtime or a temporary generation directory. Its responsibilities include PARAM, MSB, EMEVD, AI/Lua bundle, and required SFX resource handling.

The project may reuse GPL-3.0-only code from `kellernz1/ds-randomizer` with attribution and preserved notices. Source-available projects without a reuse license are behavioral and technical references only; their code is not copied.

### 5.4 RandomizerRuntime.dll

The native Windows x64 runtime component performs only behavior that requires access to the running game:

- Detect newly acquired equipment and apply the auto-equip rules
- Redirect the randomizer save to the dedicated external path
- Validate the active seed metadata after character load
- Block official login, matchmaking, session creation, and session participation
- Produce diagnostic logs without writing to the game installation

Failure to initialize save isolation or online blocking is fatal. The launcher or runtime terminates the random game before normal play begins.

## 6. External local-data layout

```text
%LOCALAPPDATA%\DSR-Randomizer\
├── runtime\
├── active-seed\
│   ├── manifest.json
│   ├── placement-hashes.json
│   └── generated game files
├── saves\
│   ├── DRAKS-RANDOM.rsl2
│   └── save-metadata.json
├── config\
├── logs\
└── staging\
```

The runtime is created locally from the user's game and is excluded from Git and release archives. The launcher verifies the source version and runtime manifest before launch. When the source game changes, only the external runtime is rebuilt.

## 7. Seed lifecycle and atomic activation

The user may type, paste, copy, or generate a seed. Text seeds are normalized into a stable internal value by a versioned algorithm.

New seed activation follows this order:

1. Verify that the random game is not running.
2. Verify the source installation without writing to it.
3. Generate all item, regular-enemy, and boss placements in `staging`.
4. Run progression, population, classification, resource, and output-integrity validation.
5. Finish every generated file and compute hashes.
6. Request final confirmation that activating the seed resets the single randomizer save.
7. Atomically replace `active-seed` with the validated staged package.
8. Reset only `saves\DRAKS-RANDOM.rsl2` and its metadata.
9. Write new seed metadata and report success.

Generation or validation failure leaves the previous active seed and save untouched. Save reset never occurs before the replacement seed is complete and verified.

## 8. Item randomization

The randomized item catalog covers:

- World pickups
- Chests
- Guaranteed and renewable enemy drops
- Merchant inventory
- NPC gifts and rewards
- Hostile and friendly NPC death drops that are valid item sources

Placement is a strict permutation. Existing items and meaningful stack quantities trade locations without arbitrary duplication or deletion. Equipment remains a single item. Shop stock and renewable-drop semantics are preserved so that moving an item does not create an infinite unique-item or soul-farming exploit.

Progression items participate in randomization. They are restricted to guaranteed finite locations and are placed by the progression solver before the remaining permutation is completed. The solver models areas, gates, required items, boss requirements, and supported non-glitch routes. It rejects any seed that cannot reach and defeat the final boss through the declared logic.

Probability-only drops are never the sole required source of a progression item. Progression items are not placed behind themselves or behind an unconditionally later requirement. The output includes an optional spoiler log and an always-generated machine-readable validation report for diagnosis.

## 9. Enemy randomization

Regular enemies and bosses use separate randomization domains. Regular-enemy slots use weighted draws with replacement; bosses use a strict compatible permutation.

### 9.1 Regular enemies

Every eligible hostile regular-enemy destination receives one deterministic weighted draw from its compatible source pool. Draws are made with replacement, so an archetype may appear several times or not appear in a given seed. The number of eligible destination slots remains unchanged, but vanilla per-archetype population counts are not preserved.

The pool includes offline-simulated special online enemies:

- Gravelord Black Phantom variants use the same spawn-weight class as elite regular enemies.
- Each supported Vagrant variant uses the same unit draw weight as an ordinary regular-enemy archetype.
- Special enemies may repeat under the same with-replacement rules as their weight class.

The catalog defines ordinary and elite weight classes explicitly so the result does not depend on map order or incidental parameter values. A versioned weight table is part of the seed format. The generator applies map resource limits, navigation and animation compatibility, linked-entity rules, and known unsafe-destination exclusions after weighting. If a drawn assignment cannot satisfy these constraints, it is deterministically redrawn from the compatible subset.

Friendly NPCs, merchants, quest actors, and invisible technical helpers remain in place. Linked bodies, detachable parts, hydra groups, scripted variants, and other inseparable entities move as groups.

Destination maps receive the required AI, effects, projectiles, models, and event adaptations. Resource-budget validation rejects placements that exceed safe simultaneous model limits. Known incompatible animation and event bindings are replaced with safe destination behavior.

### 9.2 Bosses

True boss encounters exchange only with other compatible boss encounters. Arena size, required movement mode, detachable parts, multiple bodies, phases, scripted activation, boss bars, terrain-safe spawn points, and completion flags form compatibility constraints. A boss remains one logical permutation unit even when implemented by several linked entities.

Boss randomization is treated as experimental until the full special-encounter test matrix passes. A failed special encounter is fixed in the compatibility model rather than silently removed from the randomizer.

### 9.3 Tutorial scaling

Tutorial scaling applies only during the first Undead Asylum visit:

- Randomized regular enemies in tutorial slots receive destination-appropriate early-game combat values.
- Gravelord Black Phantoms, Vagrants, and other special regular enemies drawn into tutorial slots receive the same destination-appropriate tutorial scaling.
- The randomized boss occupying the Asylum Demon tutorial encounter receives tutorial-boss combat values.
- HP, stamina, attack scaling, defenses, resistances, combat effects, and soul rewards use destination-derived values needed to keep the tutorial viable.
- The return visit and the Stray Demon encounter are not tutorial content and retain ordinary randomized source strength.
- Enemies and bosses outside the first tutorial visit retain their original source strength unless a future approved option explicitly changes that rule.

## 10. Automatic equipment

After character loading stabilizes, the runtime captures an inventory baseline. Only subsequent positive acquisition changes trigger auto-equip. Existing inventory is not mass-equipped on startup.

Rules:

- Ordinary weapons equip to right-hand slot 1.
- Shields, catalysts, pyromancy flames, and talismans equip to left-hand slot 1.
- Armor equips to its matching head, chest, hand, or leg slot.
- Rings alternate between ring slots 1 and 2.
- The next ring slot persists in `save-metadata.json`.
- Stat requirements and equip-load limits do not prevent the equip attempt.
- Consumables, keys, upgrade materials, ammunition, and other non-equipment items do not trigger auto-equip.
- An equip failure is logged and does not crash or corrupt the save.

The implementation is independent code. The Nexus `AutoEquip for Dark Souls 1 Remastered` project is used only to confirm expected observable behavior because its published source has no reuse license and its Nexus permissions require author approval for modification and redistribution.

## 11. Save isolation

The random runtime uses one save file:

```text
%LOCALAPPDATA%\DSR-Randomizer\saves\DRAKS-RANDOM.rsl2
```

It does not read or write the normal `.sl2` save. Steam Cloud must not treat the random save as the original save. `save-metadata.json` binds the save to the active seed, combined placement hash, game version, mod version, and next ring slot. A mismatch blocks launch and explains the corrective action instead of attempting an automatic conversion.

Changing the seed resets the single random save after successful generation and explicit confirmation. The first release does not maintain multiple seed saves.

## 12. Official-online blocking

The random runtime is offline-only. Defense in depth includes:

- Force the game's random-runtime network state to offline.
- Block FromSoftware service login initialization.
- Block official matchmaking discovery and registration.
- Block Steam matchmaking session creation and participation initiated by the game.
- Verify that blocking hooks are active before allowing play.
- Terminate the random process if required protection cannot initialize.
- Log every denied initialization or connection attempt.

These controls are process-local and do not change Steam, Windows firewall, the original game, or Overhaul settings. Steam may remain running for ownership verification, but the random game cannot enter official multiplayer.

## 13. Error handling and recovery

- Invalid or unsupported source version: refuse runtime creation and report the detected version and expected versions.
- Source changes during copy: discard staging and leave the previous runtime active.
- Insufficient disk space: abort before copying and report required and available space.
- Failed progression validation: discard only the candidate seed and preserve the active seed and save.
- Failed atomic activation: roll back to the previous active-seed directory.
- Save/seed mismatch: refuse launch without altering either file.
- Auto-equip address or signature mismatch: disable launch for that game version until the runtime component is updated.
- Network-block initialization failure: terminate before normal play.
- Unexpected write target: deny the write, log the canonical path, and abort the operation.

Logs redact local user-specific paths when exported for bug reports.

## 14. Testing strategy

### 14.1 Automated tests

- Stable seed normalization and independent RNG streams
- Same seed/config/version produces identical placements and hashes
- Item and boss permutation population conservation
- Regular-enemy destination-slot conservation with deterministic weighted draws and permitted archetype duplication
- Gravelord Black Phantoms use the elite weight class and Vagrants use the ordinary weight class
- Progression solver graph fixtures and impossible-seed rejection
- Progression items never use probability-only required locations
- Regular-enemy and boss pool separation
- Linked enemy and boss groups remain inseparable
- Tutorial scaling affects first-visit regular enemies and tutorial boss only
- Automatic equipment classification and exact slot rules
- Ring alternation persists across simulated sessions
- Save path redirection cannot fall back to `.sl2`
- Network initialization calls are denied in the random profile
- Canonical path guard rejects every Steam-installation descendant
- Release archive allowlist rejects game-derived files and executable extensions not built by the project

### 14.2 Seed stress tests

CI runs catalog-free deterministic and solver fixtures. Local integration tests use the user's legally owned catalog and execute large seed batches. A release candidate must complete the configured stress threshold without a population, progression, serialization, or determinism failure.

### 14.3 Local integration tests

- Snapshot hashes and timestamps for the original and Overhaul installation before a test.
- Create and launch only the external runtime.
- Exercise item acquisition, regular enemies, the tutorial boss, save/load, and blocked network initialization.
- Compare the original and Overhaul snapshot after the test; any difference is a release blocker.

## 15. Repository and release management

The GitHub repository is public from its first push and uses `GPL-3.0-only`. It contains source, tests, documentation, build scripts, and packaging manifests only.

Development uses `feat/*` and `fix/*` branches. Commits follow Conventional Commits. `main` contains reviewed, verified work. GitHub Actions builds Windows x64, runs tests, checks prohibited release content, produces a ZIP and SHA-256 checksum, and attaches them to tagged releases.

Release sequence:

- `v0.1.0-alpha.1`: launcher and external-runtime isolation
- `v0.2.0-alpha.1`: item permutation and progression validation
- `v0.3.0-alpha.1`: weighted regular-enemy placement, boss permutation, special offline spawns, and tutorial scaling
- `v0.4.0-alpha.1`: auto-equip, save isolation, and official-online blocking
- `v0.5.0-beta.1`: integrated local runtime and release-candidate testing
- `v1.0.0`: first stable release

Every release uses an annotated Git tag, a GitHub Release, release notes derived from `CHANGELOG.md`, build checksums, and third-party notices. Game files, local catalogs, saves, credentials, local paths, and generated seed packages remain excluded.

## 16. Delivery phases

1. Establish the public repository, safety guardrails, build, and test skeleton.
2. Prove that a copied external runtime can launch without loading the installed Overhaul and without modifying the source installation.
3. Implement catalog import, deterministic seed formatting, and atomic seed packages.
4. Implement item permutation and progression validation.
5. Implement weighted regular-enemy placement, offline Gravelord Black Phantom and Vagrant spawns, and first-visit tutorial scaling.
6. Implement compatible boss permutation and tutorial-boss scaling.
7. Implement auto-equip and persistent ring rotation.
8. Implement dedicated save redirection and mismatch protection.
9. Implement and verify official-online blocking.
10. Complete integration, seed stress, release-content, and original-install immutability tests.

Each phase is committed and pushed only after its applicable tests pass. Tags and releases follow the sequence in Section 15.

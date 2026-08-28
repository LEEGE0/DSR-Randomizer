# Simplified Offline Mod Runtime Design

## 1. Purpose

Build a separate, copied Dark Souls Remastered installation for offline mod use. The launcher protects the original installation, Overhaul installation, and normal save while allowing the user to add or remove ordinary mod folders manually.

This design replaces the unfinished continuous heartbeat and anti-tamper requirement with a one-time, pre-resume protection check. It preserves the already completed save, socket, Steam, game-service, profile, and suspended-launch protections.

## 2. User-visible model

The external disk contains one modded game root:

```text
<external-root>\DSR-Modded\
  Game\                 copied Dark Souls Remastered installation
  Mods\                 user-installed mod folders
  Launcher\             project launcher and native guard
  Saves\DRAKS0005.rmm   dedicated mod save
  State\                copy manifest and launch diagnostics
```

The WPF launcher exposes an external-root path and the CLI accepts the same absolute path. A single small `%LOCALAPPDATA%\DSR-Randomizer\external-root.json` pointer may remember that selection; game files, mods, saves, manifests, logs, and staging data remain on the external root.

A mod is active when its folder/files are present in the modded runtime and inactive when the user removes them. The project does not maintain an enable/disable database, checkbox list, mod load-order editor, or uninstall history. A mod that needs its own loader or configuration remains the user's responsibility and must be installed only in the copied runtime.

## 3. Isolation guarantees

- The Steam installation is a read-only source. Runtime construction copies files; it never hard-links, junctions, symlinks, or reparses files back to the source.
- The existing Overhaul installation and settings are outside every permitted write root.
- All material launcher writes remain beneath the selected external root. The only permitted local write is the bounded external-root pointer described above.
- The copied runtime has a manifest containing relative path, length, source timestamp, and SHA-256 for the clean source snapshot.
- `DarkSoulsRemastered.exe` and `steam_api64.dll` must retain the exact supported profile identities. A mod that replaces either file is unsupported and launch is denied.
- The project guard DLL, launcher binaries, compatibility profile, and dedicated `.rmm` are verified independently of user mod folders.

## 4. Save behavior

- The modded game uses only `<external-root>\DSR-Modded\Saves\DRAKS0005.rmm`.
- If `.rmm` is absent, the launcher performs the already implemented confirmed, read-only bootstrap from the selected normal `DRAKS0005.sl2`.
- If a valid `.rmm` exists, it is reused and the normal save is not reopened.
- The child process is denied access to the canonical normal DSR save root, Overhaul save suffixes, and any `.sl2` fallback.
- Deleting mod folders never deletes or resets `.rmm`.

## 5. Simplified offline boundary

The copied game starts suspended. Before its main thread resumes, the launcher injects the project-owned native guard and requires one authenticated protocol-v2 initialization result with exactly these protections active:

- `Bootstrap`
- `SaveKnownFolder`
- `SaveFileIo`
- `Winsock`
- `SteamInterfaces`
- `DeferredModuleGate`
- `GameServiceOffline`

The native guard must still:

- reject external non-loopback TCP/UDP from the copied process;
- deny profiled Steam matchmaking, P2P/networking, and Remote Storage calls;
- force the profiled game offline and deny login, discovery/session, and lobby targets;
- verify the exact executable, adjacent Steam DLL, target fingerprints, and pinned Steam interface ABI before installing hooks;
- fail closed on partial hook installation or an unsupported profile.

`Heartbeat` and `HookIntegrity` are not required by the simplified launch bitmap. The launcher does not wait for ongoing heartbeat frames and does not promise to detect a mod that deliberately tampers with protection after resume. The existing experimental monitor code may remain compiled and tested behind explicit flags, but it is not started by the simplified product path.

This is a deliberate risk trade-off accepted to shorten delivery. It protects against ordinary game/mod networking and configuration mistakes, not a hostile mod intentionally attacking the guard.

## 6. Launch lifecycle

```text
Select copied runtime
  -> verify source/runtime separation
  -> verify protected core identities
  -> prepare or reuse DRAKS0005.rmm
  -> discover present mod folders for diagnostics only
  -> create copied game suspended in a kill-on-close Job Object
  -> inject guard
  -> require exact seven-flag authenticated initialization result
  -> resume once
  -> wait for process exit
  -> close Job/process handles
```

There is no automatic mod activation database. Mod discovery records folder names and hashes only in local diagnostics; it does not transmit paths or mod names to the native guard.

## 7. Failure behavior

- A missing/modified protected core file, wrong profile, incomplete bitmap, injection failure, save redirection failure, or online-blocking hook failure terminates the Job before resume.
- Unknown or extra protection flags are rejected for the simplified launch contract.
- A user mod may modify ordinary copied data files. Such changes do not invalidate the clean-source manifest, but protected core files remain immutable.
- If the copied runtime becomes unusable, the user may delete it and rebuild it from the verified original source. The launcher does not attempt per-mod rollback.
- No failure path writes to or repairs the Steam/Overhaul installations.

## 8. Product scope

Included:

- verified copied game runtime on an external root;
- manual folder-based mod installation;
- dedicated `.rmm` save;
- one-time suspended-process offline protection verification;
- separate original, Overhaul, and modded launch paths.

Excluded:

- item, enemy, boss, or equipment randomization generation;
- mod enable/disable UI, dependency solver, load-order manager, or per-mod rollback;
- continuous heartbeat enforcement and anti-tamper guarantees;
- global Steam Offline Mode, Windows Firewall changes, or adapter changes;
- automatic execution of a real game smoke test without a later explicit user approval.

## 9. Verification and release gates

- Managed and native Debug and Release suites must pass from clean builds.
- Synthetic suspended fixtures must prove exactly one resume only after the exact seven-flag bitmap.
- Tests must reject every missing flag and every unknown extra flag.
- Integration tests must prove the original installation and normal save remain unchanged while modded runtime files and `.rmm` are writable.
- Packaging must exclude game binaries, game data, saves, local mod folders, captures, and proprietary byte sequences.
- The public launch command remains locked until the simplified integration plan passes independent review. A real copied-game diagnostic run remains a separate, explicitly approved action.

## 10. Git disposition

The incomplete Task 5 hardening edits are preserved in a named local stash and are not part of the simplified implementation. Completed safety commits remain in history. The simplified implementation is committed on `feat/official-online-guard`; publishing, merging, and tagging occur only after the simplified release gate passes.

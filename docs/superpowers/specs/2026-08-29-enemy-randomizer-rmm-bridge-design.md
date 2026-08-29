# Enemy Randomizer RMM Bridge Design

## 1. Purpose

Make Matt's DS1 Enemy Randomizer v0.1.3 load and save only the existing dedicated `DRAKS0005.rmm` when the user presses the randomizer's built-in `Launch DS1` button.

The enemy randomizer remains in its self-contained directory and continues to use its bundled Mod Engine 2 launcher and loose-file mod output. The Steam installation, normal `DRAKS0005.sl2`, and Overhaul save remain outside every permitted write path.

## 2. Confirmed environment

- External root: `D:\DSR MOD`
- Active copied runtime: resolved through `runtime-current.json`
- Selected save profile: resolved through `config\selected-save-profile.json`
- Dedicated save: `saves\<SteamID>\DRAKS0005.rmm`
- Current selected Steam ID: `146808034`
- Enemy randomizer: `runtimes\<runtime-id>\Mods\DS1 Enemy Randomizer-922-v0-1-3-1778373918\DS1EnemyRandomizer`
- Enemy randomizer launches the copied game through its bundled Mod Engine 2 fork.
- The existing project launcher already redirects `DRAKS0005.sl2` to `.rmm`, but that protection is absent when the enemy randomizer launches Mod Engine directly.

The `.rmm` is a standard 4,326,608-byte DSR save container with a private extension, not a new save format.

## 3. Constraints

- Preserve the exact `Launch DS1` user workflow.
- Do not patch or redistribute the third-party `DS1EnemyRandomizer.exe`.
- Do not move the enemy randomizer into the copied game root.
- Preserve Mod Engine loose-file loading so item and enemy randomizer merging continues to work.
- Never fall back to a normal or Overhaul `.sl2` if bridge initialization fails.
- Do not write to the Steam source installation or normal/Overhaul save roots.
- Keep the existing guarded-launch path functional.
- Treat the bundled Mod Engine ABI as pinned to the version shipped by the enemy randomizer.

## 4. Architecture

Add two project-owned components:

1. `DSRRandomizer.RmmBridge.dll`, a native x64 DLL loaded through Mod Engine's `external_dlls` list.
2. `DSRRandomizer.RmmBridgeHost.exe`, a small managed coordinator that owns the dedicated-save session and updates metadata after the game exits.

The native bridge exports `modengine_ext_init`. Mod Engine calls this symbol synchronously after `LoadLibraryW` and before its extension attach and game-file hook phases. The bridge installs the project's existing save hooks from that callback rather than from `DllMain`, avoiding loader-lock initialization.

The bridge intentionally does not depend on Mod Engine C++ class ABI. Its exported callback accepts opaque connector/output pointers, performs bootstrap, leaves the extension output null, and returns `false` after successful hook installation. Mod Engine may log the same non-extension warning already used for ordinary external DLL mods; the installed hooks remain active for the game lifetime.

## 5. Path discovery and validation

The bridge derives configuration without a hard-coded runtime ID:

1. Resolve the current process executable with `GetModuleFileNameW(nullptr, ...)`.
2. Require the leaf name `DarkSoulsRemastered.exe`.
3. Treat its parent as the copied runtime root.
4. Require the runtime parent to be `<external-root>\runtimes` and derive `<external-root>` from that layout.
5. Parse `runtime-current.json` with a strict bounded parser and require its runtime ID/path to resolve to the current process root.
6. Parse `config\selected-save-profile.json`, accepting only an ASCII decimal Steam ID of 1-20 digits.
7. Resolve and validate:
   - virtual documents: `<external-root>\profile`
   - virtual logical save: `<external-root>\profile\NBGI\DARK SOULS REMASTERED\<SteamID>\DRAKS0005.sl2`
   - normal save root: the real Documents known folder plus `NBGI\DARK SOULS REMASTERED`
   - external save root: `<external-root>\saves\<SteamID>`
   - dedicated save: `<external-root>\saves\<SteamID>\DRAKS0005.rmm`
8. Reject reparse points, multi-link aliases, unexpected file types, path escapes, missing files, wrong save length, unsupported metadata schema, and metadata/save identity mismatch.

All configuration reads are bounded. Duplicate JSON properties, unknown schema versions, invalid Unicode, relative paths, traversal, and oversized input fail closed.

## 6. Dedicated-save session host

The native bridge starts `DSRRandomizer.RmmBridgeHost.exe` from `<external-root>\components\rmm-bridge` before installing save hooks.

The bridge passes the current game PID, external root, runtime ID, Steam ID, and a cryptographically random one-time event name. The host independently validates every value against the live process and on-disk configuration. It then:

1. Opens the game process for synchronization and identity checks.
2. Acquires the existing dedicated-save session lock.
3. Verifies the `.rmm` and `save-metadata.json` identities.
4. Begins the existing `DedicatedSaveService` session, marking the save unclean before gameplay.
5. Signals the one-time ready event.
6. Waits for the game process to exit.
7. Hashes the final `.rmm`, validates its fixed length, and completes metadata with the observed exit status.

The bridge waits for either the ready event or host termination with a bounded timeout. It does not install hooks or allow gameplay if the host does not report readiness.

If the host fails after readiness, the session remains marked unclean. The next managed launcher or bridge-host preparation uses the existing abnormal-session recovery rules; it never substitutes a normal `.sl2` silently.

## 7. Save hook installation

After the session host reports readiness, the bridge calls the existing `InstallSaveHooks` with:

- `protectFileIo = true`
- `diagnosticMode = false`
- the five canonical paths described above

These hooks redirect the game's logical `DRAKS0005.sl2` operations to the dedicated `.rmm`, return the virtual Documents location, deny normal-save access, and deny Overhaul-save access.

The bridge does not enable experimental network, Steam-interface, game-service, heartbeat, or hook-integrity protections. Steam Offline Mode remains an operating prerequisite, matching the current product design.

## 8. Failure behavior

Bridge initialization is fail closed.

- Configuration, path, metadata, host, or hook failure writes a concise diagnostic under `<external-root>\logs`.
- The bridge terminates the just-created copied game process before it can load a normal save.
- It never creates or repairs a normal `.sl2`.
- It never bootstraps a missing `.rmm` from the normal save. Initial `.rmm` creation remains the managed launcher's responsibility.
- Hook cleanup occurs only on controlled test unload; process exit is the normal production cleanup boundary.

## 9. Enemy Randomizer integration

Deploy the bridge components beneath:

```text
D:\DSR MOD\components\rmm-bridge\
  DSRRandomizer.RmmBridge.dll
  DSRRandomizer.RmmBridgeHost.exe
  supporting managed files, if framework-dependent publishing requires them
```

Configure the enemy randomizer's optional other-mod path to this directory and enable its DLL-mod merge option. Keep `Merge files from game directory` enabled when item-randomizer output must be preserved.

After each `Randomize!`, verify the generated `config_randomizer.toml` contains the bridge DLL in `modengine.external_dlls` and does not contain `DarkSoulsItemRandomizer.exe`. The configuration file is auto-generated and is not treated as a durable source file.

For immediate local deployment, update the current generated TOML only after preserving a byte-for-byte backup. Future regeneration is driven by the enemy-randomizer UI selection above.

## 10. Testing

### Native tests

- Strict external-root/runtime/profile path discovery.
- JSON rejection cases and path-escape/reparse rejection.
- Missing, malformed, wrong-length, linked, or mismatched `.rmm` rejection.
- `modengine_ext_init` installs hooks synchronously outside `DllMain`.
- Initialization failure reaches the injected termination abstraction in tests.
- Existing `SaveHookIntegrationTests` remain green.

### Managed host tests

- Live process identity and runtime binding validation.
- Ready-event handshake only after session acquisition.
- Metadata becomes unclean before readiness.
- Normal and abnormal process exits update metadata and the final `.rmm` hash correctly.
- Host/config/session failures occur before readiness.
- Concurrent guarded launcher and bridge sessions are rejected.

### Integration tests

- A synthetic Mod Engine-style loader calls `LoadLibraryW` and `modengine_ext_init` before a fixture attempts `DRAKS0005.sl2` access.
- The fixture reads and writes only the dedicated `.rmm`.
- Normal and Overhaul saves remain byte-for-byte unchanged.
- Missing host, missing `.rmm`, invalid metadata, and hook failure terminate the fixture before fallback.
- Existing managed and native Debug/Release suites pass.

### Local deployment verification

- Built artifact hashes are recorded before copying.
- Deployed artifact hashes match the build output.
- Generated TOML contains the bridge DLL and required heap patch, but no executable masquerading as a DLL.
- A real-game smoke launch is not automated as part of build verification; the user can press `Launch DS1` after Steam is placed in Offline Mode.

## 11. Rollback

Rollback removes the bridge from the enemy randomizer's DLL-mod selection and regenerates `config_randomizer.toml`. Deployed bridge components may remain inert or be removed after verifying they are not loaded. No game, enemy-randomizer, normal-save, or `.rmm` restoration is required.

## 12. Source and licensing notes

The bridge is project-owned and reuses project-owned save redirection code. It consumes only the public Mod Engine callback convention and does not include or modify Matt's enemy-randomizer binary. Mod Engine 2 public architecture and extension sources are MIT-licensed and serve as the ABI reference. Local integration does not distribute a fork of the enemy randomizer.

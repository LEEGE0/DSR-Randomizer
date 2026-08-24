# Native Safety Runtime Design

Date: 2026-08-24
Status: Proposed written specification, pending user review

## 1. Purpose

This subsystem is the mandatory safety boundary between the DSR Randomizer launcher and the copied Dark Souls Remastered runtime. It allows the launcher to start only the user-local copied executable, redirects that process to a dedicated `.rmm` save, and prevents that process from entering official online services.

The public **Launch Random Dark Souls** control remains locked until a validated active seed exists. This subsystem may perform a short, automatically terminated diagnostic smoke launch after all protection checks pass, but it does not make the current alpha generally playable.

## 2. Decisions

- Launch the copied game as a suspended x64 child process and inject a project-owned native x64 guard DLL before any game code runs normally.
- Initialize hooks through an exported function after `LoadLibraryW`; `DllMain` performs no hook installation or other loader-lock-sensitive work.
- Resume the game main thread only after the native guard reports that every required protection is installed and verified.
- Redirect the game's `DRAKS0005.sl2` access to one external `DRAKS0005.rmm` file.
- Bootstrap the `.rmm` by copying the user's normal `DRAKS0005.sl2` once. If a valid matching `.rmm` already exists, use it and do not reopen the normal save.
- Block official networking inside the copied game process. Do not change Windows Firewall, Steam settings, the normal game, or Overhaul.
- Terminate the copied game if initialization, heartbeat, save protection, or online protection becomes unhealthy.
- Support only exact, explicitly profiled game builds. Unknown executable hashes fail closed.

The rejected alternatives are a `d3d11.dll` proxy in a game directory and system-wide firewall or Steam-offline changes. A proxy is too easy to confuse with or collide with Overhaul, while system-wide settings mutate shared state and cannot prove that the copied process is protected.

## 3. Scope

### 3.1 In scope

- Suspended-process creation, DLL injection, initialization handshake, and fail-closed resume
- Exact executable compatibility profiles
- Dedicated `.rmm` save bootstrap, validation, redirection, metadata, and seed binding
- Process-local official-online blocking at game, Steam, and socket layers
- Job Object supervision, heartbeat, diagnostics, and controlled smoke testing
- Native build integration, dependency notices, automated tests, and packaging guards

### 3.2 Out of scope

- Item, enemy, or boss randomization
- Auto-equip behavior
- A custom multiplayer service
- Launch buttons for the normal game or Overhaul
- Any write, rename, replacement, patch, or proxy installation in the Steam game directory
- Any write to the normal or Overhaul save
- Any attempt to evade anti-cheat or support official online play

## 4. Safety invariants

The following invariants are release blockers, not preferences:

1. The executable started by this subsystem is beneath the verified external runtime root and is never the executable beneath the Steam installation root.
2. The Steam installation and installed Overhaul remain read-only inputs. The launch path does not load the installed Overhaul proxy or companion files.
3. The normal save may be read only during an explicit first bootstrap or confirmed seed reset. It is never opened for write, rename, replacement, truncation, metadata update, or deletion.
4. The production launcher and copied game never open the Overhaul save. A separately invoked immutability audit may hash it read-only before and after a diagnostic test.
5. A matching existing `.rmm` takes precedence during ordinary startup; the normal `.sl2` is not opened in that path.
6. The copied game cannot resume if save redirection or any required online block is absent or unverifiable.
7. Protection failure after resume terminates the entire copied-game process tree.
8. All runtime writes remain beneath `%LOCALAPPDATA%\DSR-Randomizer`. The installed launcher package, Steam installation, and copied runtime files outside declared mutable data are read-only during launch. No runtime log is written to a game directory.
9. Steam Cloud never receives or manages `DRAKS0005.rmm`.

This design protects against accidental save contamination, accidental Overhaul loading, and accidental official-network entry by the supported game process. It is not a sandbox for hostile injected code or malware.

## 5. External data layout

```text
%LOCALAPPDATA%\DSR-Randomizer\
├── runtime\
│   └── versions\<runtime-id>\DarkSoulsRemastered.exe
├── components\
│   └── <mod-version>\DSRRandomizer.Runtime.dll
├── profile\
│   └── Documents\
├── saves\
│   └── <SteamID>\
│       ├── DRAKS0005.rmm
│       └── save-metadata.json
├── config\
│   └── compatibility-profiles.json
├── logs\
└── staging\
```

`<SteamID>` is a decimal directory name discovered from the normal DSR save layout and confirmed by the user when more than one candidate exists. It is stored as data and is never committed or printed in exported logs. The launcher does not choose an arbitrary candidate.

The dedicated save path is therefore:

```text
%LOCALAPPDATA%\DSR-Randomizer\saves\<SteamID>\DRAKS0005.rmm
```

The `.rmm` extension deliberately prevents the file from being mistaken for a normal DSR or Overhaul save. The game does not see this physical path directly; the guard maps its expected save request to it.

## 6. Components

### 6.1 Safety launch coordinator

The managed launcher owns the state machine and all Windows process handles. Its responsibilities are:

- Revalidate the external runtime manifest and canonical executable path.
- Match the copied executable SHA-256 to one compatibility profile.
- Resolve the dedicated save state without modifying a valid existing `.rmm`.
- Create a nonce-authenticated local control channel.
- Create and configure a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and no breakaway permission.
- Call `CreateProcessW` with `CREATE_SUSPENDED`, a minimal explicit environment, the external runtime as the working directory, and no shell mediation.
- Assign the child to the Job Object before injection.
- Inject the exact guard DLL selected from the verified launcher package.
- Invoke the exported native initializer and validate its structured result.
- Resume the original main thread exactly once only after every check succeeds.
- Monitor heartbeat and terminal protection events until the child exits.

The coordinator retains the original process and thread handles. It never searches by process name to decide which process to resume or terminate.

### 6.2 Native guard DLL

`DSRRandomizer.Runtime.dll` is a project-owned Windows x64 DLL written in C++ and built with MSVC. `DllMain` only records the module handle and disables unnecessary thread notifications. The launcher calls an exported `InitializeProtection` entry point on a separate remote thread after `LoadLibraryW` completes.

The initializer receives only a versioned configuration block containing canonical external paths, the compatibility-profile identifier, the control-channel nonce, and feature flags for diagnostic or playable mode. It installs save, Steam, game-network, and Winsock hooks using MinHook v1.3.4, verifies every target and trampoline, starts the control heartbeat, and returns one structured status code.

No partial-success state is resumable. On any failure, installed hooks are disabled, the failure is reported when possible, and the launcher terminates the Job Object.

### 6.3 Compatibility profile

Each supported game build has a release-pinned, project-authored profile containing:

- PE machine type, image size, timestamp, and SHA-256
- Required module names and minimum/maximum image ranges
- Version-specific FromSoftware login/offline targets
- Steam interface versions and required methods
- Expected instruction bytes or function fingerprints at every internal target
- Post-install verification rules

The first local profile targets the currently verified Steam x64 executable SHA-256:

```text
a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b
```

A hash match alone does not bypass target-byte verification. A mismatch at either level blocks launch and reports that the game build is unsupported. Profiles never use wildcard signatures that can silently match several functions.

### 6.4 Deferred module gate

Some Steam implementation modules may not be mapped when the child is first created. The guard therefore installs a module-load and symbol-resolution gate before resume. Immediate protection of the main game image, save APIs, known-folder APIs, Winsock, and every already loaded Steam target is mandatory. An unloaded profiled module is marked `armed-deferred`, not `installed`.

When an armed module loads, the calling game thread cannot receive its module handle or resolved protected symbol until the guard has checked the module path/hash/interface version and installed the dependent hooks. A failed check terminates the process before the protected call returns. The launch protection bitmap distinguishes installed hooks from armed gates, and a profile explicitly declares which targets may be deferred. Game-service targets in the main executable, save redirection, socket containment, and supervisor integrity can never be deferred.

### 6.5 Supervisor channel

The launcher and guard communicate through a per-launch Windows named pipe whose name contains a 256-bit cryptographically random nonce. The channel carries:

- Guard initialization result and installed-protection bitmap
- Selected compatibility-profile identifier
- Save target hash and metadata state
- Heartbeat sequence and monotonic timestamp
- Denied network-operation counters
- Fatal protection events

The pipe ACL permits only the current Windows user and LocalSystem. The guard never accepts commands from a client that cannot prove the launch nonce. Logs are informational; the authenticated live state is authoritative. The guard emits one heartbeat per second; five consecutive missing heartbeats close the Job Object and terminate the copied game.

## 7. Launch state machine

```text
Locked
  -> ValidateExternalRuntime
  -> SelectCompatibilityProfile
  -> ResolveDedicatedSave
  -> CreateSupervisor
  -> CreateSuspendedChild
  -> AssignJobObject
  -> InjectGuard
  -> InitializeAndVerifyAllProtection
  -> ResumeMainThread
  -> Monitor
  -> CleanExit
```

Any transition may move to `TerminateAndReport`. There is no transition from a failed or partially initialized state to `ResumeMainThread`.

Injection proceeds as follows:

1. Allocate remote memory for the canonical DLL path and initialization block.
2. Write and read back both regions to verify their contents.
3. Start and wait for a bounded remote call to `LoadLibraryW`.
4. Enumerate the child modules by canonical path to obtain the full-width remote module base, then add the RVA of `InitializeProtection` parsed from the verified DLL on disk. The 32-bit remote-thread exit code is never treated as a 64-bit module pointer.
5. Start a bounded remote call to that exported initializer.
6. Receive the detailed result through IPC and cross-check the remote call result.
7. Release temporary remote allocations.
8. Resume the original main thread only when the required-protection bitmap exactly matches the compatibility profile.

Timeout, child exit, unexpected module, IPC disagreement, or suspend-count inconsistency terminates the process. The launcher does not retry inside the same child.

## 8. Dedicated `.rmm` save lifecycle

### 8.1 Source discovery

The normal source candidate is shown below. The launcher obtains the Documents directory through `FOLDERID_Documents`; it does not construct the physical path from `%USERPROFILE%` when Windows has redirected Documents.

```text
%USERPROFILE%\Documents\NBGI\DARK SOULS REMASTERED\<SteamID>\DRAKS0005.sl2
```

Discovery enumerates only numeric SteamID directories and exact `DRAKS0005.sl2` names. Files with Overhaul suffixes, backups, temporary names, or other extensions are excluded. When zero candidates exist, bootstrap stops and asks the user to create a normal-game save first. When several candidates exist, the launcher requires an explicit selection and remembers only the selected SteamID and canonical source path.

### 8.2 First bootstrap

If `DRAKS0005.rmm` does not exist:

1. Confirm the exact source and external destination in the launcher.
2. Open the normal `.sl2` with read access and restrictive sharing; never request write or delete access.
3. Snapshot source length, last-write time, file identity, and SHA-256.
4. Copy bytes to a uniquely named file beneath external `staging`.
5. Flush the staged file and verify its length and SHA-256 against the source snapshot.
6. Recheck source identity, length, and last-write time to detect a concurrent save update.
7. Atomically move the verified staged file to `DRAKS0005.rmm` only when the destination is still absent.
8. Write `save-metadata.json` atomically beneath the same external SteamID directory.
9. Reopen and hash the `.rmm`; report success only when it still matches the source snapshot.

The bootstrap never modifies file attributes or timestamps on the source. Failure removes only the staged external copy. It never deletes or repairs the source.

### 8.3 Existing `.rmm`

When `.rmm` exists, ordinary startup does not open the normal `.sl2`. The launcher validates:

- The destination is a regular file beneath the canonical external saves root and is not a link or reparse escape.
- Metadata schema and SteamID match.
- The save length equals the fixed allowed length in the compatibility profile and the bootstrap length recorded in metadata.
- The active-seed identifier and placement hash match, once the seed subsystem exists.
- The stored last-known `.rmm` hash matches when the previous session exited cleanly.

A failed validation blocks launch and preserves the `.rmm` for diagnosis. The launcher never silently replaces it from the normal save.

### 8.4 Seed reset

Ordinary launcher restarts reuse the existing `.rmm`. Activating a different validated seed is the only normal reset path. After explicit confirmation, the launcher archives the prior `.rmm` beneath the external saves root, repeats the read-only bootstrap from the exact selected normal `.sl2`, binds the new `.rmm` metadata to the new seed, and then atomically activates both seed and save. Failure restores the previous external seed/save pair.

Until seed generation exists, diagnostic mode records an unbound-save metadata state. Public gameplay remains locked, so that diagnostic state cannot be used for a normal play session.

### 8.5 In-process redirection

The child receives a virtual Documents directory beneath the external profile root. The guard intercepts the supported known-folder resolution paths and the file operations used by the profiled game build. The exact logical request ending in:

```text
NBGI\DARK SOULS REMASTERED\<SteamID>\DRAKS0005.sl2
```

is mapped to external `DRAKS0005.rmm` before any open occurs. Reads, writes, flushes, size changes, renames, replacements, deletes, attribute queries, and directory enumeration required by the game operate on the external target or its external transaction files.

Defense in depth rules deny:

- Any child access to the canonical real DSR save root after bootstrap
- Any path containing the installed Overhaul save suffix
- Any normal `.sl2` target outside the external virtual profile
- Reparse-point or case/short-name paths that escape the external save root
- Any fallback to the real path after a redirected operation fails

Steam Remote Storage and cloud-save operations for this title are denied in the copied process. The `.rmm` path is never presented to Steam as a cloud file.

## 9. Official-online blocking

Protection uses three independent layers because no single API proves that the game is offline.

### 9.1 Game-service layer

The exact compatibility profile forces the copied game into its offline state before resume and blocks FromSoftware login initialization, discovery, registration, session creation, and session participation. Each internal target is verified against its expected fingerprint before patching or detouring. A missing target is fatal.

### 9.2 Steam layer

Steam may remain running for ownership verification and basic `SteamAPI_Init`. The guard denies the profiled game's use of:

- Steam Matchmaking lobby creation, search, join, and advertisement
- Steam Networking and P2P session creation, acceptance, send, and receive paths
- Steam Remote Storage/cloud reads, writes, synchronization, and file publication for the random profile

The guard validates the Steam interface versions returned at runtime. An unknown interface version fails closed instead of assuming compatible virtual-table indices.

### 9.3 Socket layer

As a final containment layer, the guard denies non-loopback outbound socket connection and datagram-send paths used by the child, including dynamically obtained extension functions. Loopback is allowed only for the authenticated launcher control channel or test fixtures. A compatibility profile must prove that required Winsock entry points are hooked before resume.

Every denied attempt increments a counter and emits a redacted event. Repeated attempts do not weaken the block. A detected hook/trampoline removal, unexpected mutation outside the guard's declared patch bytes, or protection-thread failure is fatal.

## 10. Error handling

| Condition | Required behavior |
|---|---|
| Unsupported executable or target fingerprint | Do not create or resume the child; name the unsupported build hash |
| `.rmm` absent and normal `.sl2` absent | Ask the user to create a normal save; change nothing |
| Multiple normal-save profiles | Require explicit SteamID selection |
| Valid matching `.rmm` exists | Use it without opening normal `.sl2` |
| Invalid or seed-mismatched `.rmm` | Block launch; preserve file and explain the mismatch |
| Source changes during bootstrap | Delete only staged external copy and retry only after user action |
| Destination appears during bootstrap | Preserve both; validate the destination on the next attempt |
| Guard injection or initialization fails | Terminate Job Object before resume |
| Required online hook fails | Terminate Job Object before resume |
| Protection fails after resume | Terminate process tree and mark session unclean |
| Launcher exits or crashes | Job Object closes and kills copied-game process tree |
| Copied game exits normally | Flush `.rmm`, update external metadata atomically, close supervisor |

User-facing errors include a stable error code, supported remediation, and a redacted diagnostic-log path. They never recommend modifying or deleting the normal or Overhaul installation.

## 11. Native build and licensing

The native subtree is built for Windows x64 with the MSVC C++ toolchain and CMake, then integrated into the existing solution and Windows CI. Local implementation requires Visual Studio Build Tools with the Desktop development with C++ workload and a pinned CMake version. CI uses the equivalent pinned Windows runner/toolset combination.

MinHook v1.3.4 is pinned by version and source hash. Its BSD-2-Clause notice is included in `THIRD_PARTY_NOTICES.md` and the release archive. No source or binary from Dark Souls Overhaul is linked, copied, or redistributed. Overhaul's public AGPL source may be inspected only as a behavioral compatibility reference.

The project does not distribute game executables, saves, symbols, signatures derived from proprietary byte sequences beyond the minimum non-expressive fingerprints required for compatibility, or locally extracted game data.

## 12. Testing strategy

### 12.1 Managed unit tests

- Launch state machine has no failure-to-resume transition.
- Canonical path checks reject the Steam executable and every external-root escape.
- Exact hash selects one profile; unknown and ambiguous profiles are rejected.
- Existing valid `.rmm` path performs zero normal-save opens.
- First bootstrap opens normal `.sl2` read-only and produces a byte-identical `.rmm`.
- Concurrent source change, destination race, short write, hash mismatch, and reparse escape preserve source and existing destination.
- Invalid/mismatched `.rmm` is never automatically overwritten.
- Job Object, process, thread, and remote-allocation handles are closed on every failure path.

### 12.2 Native tests

A project-owned native harness loads the guard into a harmless x64 fixture process. Tests prove:

- `DllMain` does not install hooks.
- Initialization is all-or-nothing and reports the exact failed protection.
- Known-folder and file redirection map only the declared logical save.
- Normal and Overhaul save paths are denied without fallback.
- External `.rmm` read/write/rename/flush behavior remains correct.
- Steam matchmaking, networking, and remote-storage fixture calls are denied.
- Non-loopback TCP and UDP fixture calls are denied while authenticated loopback remains available.
- Hook-integrity or heartbeat loss produces a fatal supervisor event.

Native tests run under CTest in CI and locally before any real-game smoke test.

### 12.3 Injection integration tests

A harmless suspended x64 fixture executable validates remote `LoadLibraryW`, exported initialization, result cross-checking, exact main-thread resume count, Job Object cleanup, timeout behavior, and launcher-crash termination. Negative fixtures simulate wrong architecture, wrong target bytes, partial hook failure, IPC spoofing, and child exit during injection.

### 12.4 Controlled copied-game smoke test

The first real-game launch is allowed only after managed, native, injection, packaging, and release-content tests pass. It uses only the external runtime and a diagnostic switch unavailable from the public play button.

Before launch, the explicitly invoked diagnostic audit records hashes, sizes, and timestamps for:

- Every file in the original Steam installation
- The selected normal `.sl2`
- Every detected Overhaul save
- The installed Overhaul proxy/configuration files

The launcher bootstraps or validates `.rmm`, starts the copied executable suspended, verifies all protections, resumes it for at most 30 seconds, observes protection heartbeat and denied-network counters, and terminates it automatically through the Job Object. Network observation must show no non-loopback traffic from the copied process. File auditing must show writes only beneath the external DSR Randomizer root.

After exit, the snapshots must prove:

- Original game and Overhaul installation changed files: zero
- Normal `.sl2` content, size, and timestamps changed: zero
- Copied-game and production-launcher Overhaul save reads and writes: zero; only the separate read-only audit may hash it
- Dedicated `.rmm` exists externally and is the only game save opened for write
- Official connection attempts that occurred were denied before leaving the process

Any mismatch is a release blocker. The smoke switch remains disabled in distributed builds until its explicit release phase.

### 12.5 Packaging tests

The release guard allowlists the project-built native DLL and its debug-symbol policy, requires its SHA-256 manifest and notices, and continues to reject game binaries, game data, local saves, credentials, compatibility captures, and locally generated profiles.

## 13. Delivery sequence

1. Add native toolchain detection, pinned MinHook acquisition, native fixture, and CTest integration.
2. Add the managed launch state machine, Job Object wrapper, and harmless-fixture suspended injection tests.
3. Implement `.rmm` discovery, first bootstrap, metadata, validation, and transactional reset with managed filesystem doubles.
4. Implement native virtual-Documents and `.rmm` redirection against fixtures.
5. Implement Winsock containment against fixtures.
6. Implement exact Steam and FromSoftware compatibility-profile hooks and verification.
7. Integrate authenticated heartbeat, protection bitmap, hook-integrity monitoring, and fatal supervision.
8. Extend packaging, notices, CI, and prohibited-content tests.
9. Run the controlled copied-game smoke test and immutability audit.
10. Keep the public launch control locked until a validated seed package can be bound to `.rmm` metadata.

Each step is test-driven and committed only after its applicable verification passes. The implementation plan must split these steps into independently reviewable commits and must not combine the first real-game smoke launch with unfinished hook work.

## 14. Acceptance criteria

This subsystem is complete only when:

- A valid external runtime can be launched suspended and cannot resume without the full required-protection bitmap.
- A first-time user can copy an exact normal `DRAKS0005.sl2` into external `DRAKS0005.rmm` without changing the source.
- A subsequent valid startup uses existing `.rmm` without opening normal `.sl2`.
- The supported game process writes only the external `.rmm` and cannot access an Overhaul save; the production launcher also never opens the Overhaul save.
- Official FromSoftware, Steam matchmaking/networking, Steam Cloud, and non-loopback socket paths are blocked and monitored.
- Launcher or guard failure terminates the copied-game process tree.
- Automated tests and the bounded smoke test prove zero changes to the original game, normal save, and Overhaul environment.
- The distributed launcher still keeps normal gameplay locked until an active validated seed exists.

## 15. Technical references

- [Microsoft: CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Microsoft: Process creation flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
- [Microsoft: Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [MinHook](https://github.com/TsudaKageyu/minhook)
- [Souls Modding: SL2 files](https://sites.google.com/view/soulsmods/file-formats/sl2-files)
- [Mod Engine 2 support statement](https://github.com/soulsmods/ModEngine2/blob/main/README.md)
- [Dark Souls 1 Overhaul public source](https://github.com/metal-crow/Dark-Souls-1-Overhaul)

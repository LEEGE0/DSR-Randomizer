# Overhaul GameParam Redirect Design

**Status:** Implemented. The committed release uses DSR for MOD's launcher-owned `Launch modded copy` entrypoint; direct Enemy Randomizer `Launch DS1` wording in the original proposal is superseded.

## 1. Purpose

Make the integrated launcher workflow load one generated
`GameParam.parambnd.dcx` that combines the Steam Overhaul data with the active
item- and enemy-randomizer output.

The generated file lives under `<external-root>`. The Steam installation is a
read-only input and is never modified.

## 2. Confirmed cause

The live game process requests `./overhaul/GameParam.parambnd.dcx`. The enemy
randomizer emits its combined item/enemy result under its own
`param/GameParam/GameParam.parambnd.dcx`, but the bundled Mod Engine loose-file
lookup does not substitute that file for this relative Overhaul request. The
game therefore reads the Steam Overhaul binder and misses the randomizer
changes.

## 3. Inputs and output

At every bridge-host launch, resolve exactly one enemy-randomizer directory
below the active runtime's `Mods` directory and use:

- Base: `dist1/Vanilla/GameParam.parambnd.dcx`
- Randomized: `param/GameParam/GameParam.parambnd.dcx`
- Overhaul: `<verified Steam installation>/overhaul/GameParam.parambnd.dcx`
- Output: `<external root>/components/rmm-bridge/content/overhaul/GameParam.parambnd.dcx`

The base and randomized inputs must belong to the same resolved enemy-randomizer
directory. The Steam Overhaul path must resolve beneath the verified source
installation. The output must resolve beneath the external root.

## 4. Merge semantics

Perform a three-way merge at binder-entry and PARAM-row granularity:

1. A binder entry unchanged between base and randomized preserves the Overhaul
   entry byte-for-byte at the logical data level.
2. A binder entry added or removed by the randomized result applies that
   addition or removal to the Overhaul binder.
3. For a changed PARAM entry, compare rows by row ID.
4. A row unchanged between base and randomized preserves the matching Overhaul
   row.
5. A row added, changed, or removed by the randomized result applies that
   change to the Overhaul PARAM.
6. Randomizer changes win when both randomized and Overhaul changed the same
   row from base.
7. Unrelated Overhaul binder entries and rows remain intact.
8. Binder metadata, PARAM format metadata, row ordering, names, and DCX
   compression must remain valid for Dark Souls Remastered.

Changed PARAM entries are parsed with exactly one compatible Enemy Randomizer
PARAMDEF selected by exact `ParamType` and detected row size. Cell values are
compared bit-exactly, while nonfunctional row names do not create a change.
The merger preserves target container/header metadata and applies row additions,
changes, and deletions according to the rules above.

## 5. Generation and validation

Generation happens in the managed bridge host before the dedicated-save
session signals readiness. It must:

- reject zero or multiple enemy-randomizer directory matches;
- reject missing, reparse, linked, malformed, incompatible, or escaping input
  and output paths;
- parse all three binders and every changed PARAM involved in the merge;
- write to a unique staging file below the external root;
- reopen and parse the staging file;
- verify the staged logical merge against the three inputs;
- atomically replace the output only after verification;
- record source hashes, output hash, merge counts, and failures under
  `<external root>/logs`;
- rebuild when source hashes change, including after either randomizer is
  rerolled;
- fail closed before game readiness if generation or validation fails.

No temporary or final file may be written to the Steam installation or a save
directory.

## 6. Native read redirect

Extend the existing bridge-owned file-hook path so the live game redirects only
read-compatible opens of the exact logical Overhaul GameParam request to the
generated output.

Accepted source spellings are:

- the relative normalized path `overhaul/GameParam.parambnd.dcx`; and
- the equivalent absolute path below the verified live Steam game root.

Matching is case-insensitive and separator-insensitive after lexical
normalization. Names with extra components, alternate filenames, traversal,
device namespaces, ADS syntax, or paths outside the verified game root do not
match.

The redirect applies only when requested access and disposition cannot modify,
truncate, delete, or create the file. Write-like opens pass through the normal
save/file protection policy and are never redirected to the generated file.
Relevant existence/attribute probes must observe the generated file consistently
with the read redirect.

The implementation must reuse the existing hook installation rather than
install a competing `CreateFile` detour.

## 7. Failure and safety behavior

- Missing or invalid generated content aborts RMM bridge bootstrap.
- The bridge never falls back to silently loading an unmerged GameParam after
  it has been selected for this workflow.
- Existing dedicated `.rmm` save isolation and guarded-launch behavior remain
  unchanged.
- Normal and Overhaul `.sl2` saves remain inaccessible to the launched game.
- Steam files are sampled before and after real-game verification and must be
  byte-for-byte unchanged.
- The `.rmm` body must not change during generation-only verification.

## 8. Verification boundary

Automated tests cover merge semantics, malformed/incompatible input, atomic
publication, strict path matching, read-only access filtering, bootstrap
ordering, and existing save-hook regressions.

Deployment verification builds and hashes the managed/native artifacts and
the generated GameParam under `<external-root>`. A final real-game smoke launch may
be performed through DSR for MOD only after automated verification;
its evidence must include bridge logs showing the source hashes, output hash,
and a successful redirect count.

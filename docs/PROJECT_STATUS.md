# DSR Randomizer Project Handoff

Snapshot created: 2026-08-28 (Asia/Seoul). This file is not automatically maintained; verify the active plan, SDD ledger, and `git log` for newer progress.

## How to continue in another Codex chat

Ask the new chat to read this file first:

`C:\Users\User\Documents\DSR-Randomizer\.worktrees\official-online-guard\docs\PROJECT_STATUS.md`

Then ask it to continue the implementation plan at:

`C:\Users\User\Documents\DSR-Randomizer\.worktrees\official-online-guard\docs\superpowers\plans\2026-08-28-simplified-offline-mod-runtime.md`

The active repository/worktree is:

- Repository: `C:\Users\User\Documents\DSR-Randomizer`
- Worktree: `C:\Users\User\Documents\DSR-Randomizer\.worktrees\official-online-guard`
- Branch: `feat/official-online-guard`
- Remote: `git@github.com:LEEGE0/DSR-Randomizer.git`

## Confirmed product scope

Build a launcher for a separately copied Dark Souls Remastered runtime on an external disk. The copied runtime accepts third-party mods installed or removed manually. The launcher must keep the normal game, an existing Overhaul setup, and this modded copy independently launchable.

The simplified product does not generate randomized items/enemies/auto-equipment and does not maintain a mod enable/disable database. Folder/file presence controls whether a user-installed mod is present.

The launcher must:

- Treat the Steam installation as a read-only copy source.
- Never modify the normal game, Overhaul, normal `.sl2`, Steam settings, Windows Firewall, or network-adapter settings.
- Use a dedicated `DRAKS0005.rmm`; if it already exists, use it. Save bootstrap from `DRAKS0005.sl2` remains a separate explicit operation.
- Launch only the exact pinned copied executable and adjacent `steam_api64.dll`.
- Require exactly the seven core offline protection flags (`0x7F`) before process resume.
- Close the authenticated initialization pipe after the one-shot check; heartbeat/hook-integrity monitoring is experimental and not part of the simplified product path.
- Keep all material data under the selected external root. Only `%LOCALAPPDATA%\DSR-Randomizer\external-root.json` may be a small local pointer.
- Never start the real game during implementation tests unless the user separately authorizes it.

## Design and implementation documents

- Approved design: `docs/superpowers/specs/2026-08-28-simplified-offline-mod-runtime-design.md`
- Active plan: `docs/superpowers/plans/2026-08-28-simplified-offline-mod-runtime.md`
- Ignored SDD ledger: `.superpowers/sdd/2026-08-28-simplified-offline-mod-runtime/ledger.md`
- Incomplete pre-simplification Task 5 work is preserved in `stash@{0}`. Do not drop or apply it without a new decision.

## Current progress

- `da937fe` — simplified offline mod runtime design.
- `73ab2c4` — four-task implementation plan and preflight rulings.
- `b06e760` — Task 1 exact one-shot `0x7F` native/managed protection contract implemented.
- Task 1 implementation verification passed: managed Debug/Release 198/198 each; native Debug/Release 11/11 each; protocol block remains 5480 bytes.
- Task 1 independent review is currently in progress. Do not mark Task 1 approved until its review report says `APPROVED`.

Remaining ordered tasks:

1. Finish and resolve Task 1 independent review.
2. Implement and independently review mod-ready copied-runtime validation while preserving strict clean audit.
3. Implement and independently review validated external-root selection/pointer behavior.
4. Connect CLI/WPF launch, package the guard/profile, run all gates, and obtain a broad final branch review.

## Operational rules

- Use the isolated worktree above, not the Steam game directory.
- Preserve unrelated user changes and `stash@{0}`.
- Use synthetic fixtures only; do not run `DarkSoulsRemastered.exe`.
- Do not push, publish, tag, merge, change firewall/Steam settings, or mutate real saves without separate confirmation at the point of that external or irreversible action.
- Treat this file as a handoff snapshot. Do not spend implementation time updating it automatically.

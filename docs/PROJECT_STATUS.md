# DSR Randomizer handoff pointers

This file intentionally contains only stable handoff pointers and is not a status snapshot.

- Worktree: use the current checkout returned by `git rev-parse --show-toplevel`; no user-specific path is recorded here.
- Branch: `feat/official-online-guard`
- Authoritative release design: `docs/superpowers/specs/2026-08-31-redistributable-release-design.md`
- Release implementation plan: `docs/superpowers/plans/2026-08-31-redistributable-release.md`
- Local SDD review ledger: `.superpowers/sdd/2026-08-31-redistributable-release/progress.md` (ignored, machine-local evidence)

Read `git log --oneline --decorate` and the SDD ledger for current progress, decisions, review status, and commit identities.

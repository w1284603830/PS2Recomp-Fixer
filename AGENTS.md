# Reusable-base contract

PS2Recomp-Fixer is a game-independent base for anyone building a PS2 static-recompilation port. Shared components
implement guest hardware and host services; each port supplies its own game/revision profile, assets and entry points.
Apply this contract to changes throughout the repository.

## Separate shared mechanisms from game profiles

- Classify a change before integrating it: hardware semantics, generic host service, SDK/library ABI adapter, game
  profile, or game content. Keep these boundaries explicit in code and documentation.
- Hardware fixes need a documented hardware/upstream reference and focused tests. A successful run of one game
  establishes that run's coverage; it does not establish compatibility with all games.
- Guest addresses, instruction guards, library work layouts, overlay identities, movie tables, texture exclusions,
  HUD/depth conventions and GameDB settings belong to explicit game/revision profiles or reference runners.
- Default shared behavior must remain neutral. Do not enable a game's workaround just because another game's
  renderer settings happen to match. Reset profile state when a new game/session opens.
- Unknown SDK/library ABIs need an explicit supported adapter, a validated native path, or a clear unsupported
  result. A branch named "generic" or "non-DoD" is not evidence of a portable ABI.
- Historical `dod` names may refer to shared mechanisms. Inspect behavior and call sites before changing them;
  avoid global renames and preserve documented environment aliases.

## Particle-edge workarounds: every game needs qualification

This rule applies to every game and revision, including a reference game whose earlier build was tested.
The reusable part is the opt-in/profile mechanism and diagnostic tooling, not a universal set of sprite rules.

- Keep a workaround disabled until the port explicitly selects a qualified profile. A profile must specify the
  applicable HPO/vertex-grid relationship, draw qualification, texture sampling rules and HUD/depth exclusions.
- Never promote a game's depth threshold, atlas layout, HPO value or clamp rule into a universal default.
- An HPO4/HPO5 mismatch can explain why a draw was skipped; removing that guard alone does not prove that the
  underlying coordinate transform is correct for the new game.
- Separate atlas-border sampling from alpha/depth/occlusion problems. Record why each draw was applied or skipped.
- Adjacent-frame A/B captures can differ in animation and render-target contents. Use frozen-state replay where
  needed; verify that the tested draws actually exercised the candidate path.
- Revalidate affected particles, HUD/text, render-target effects, native/upscaled output and directly related paths.
  Preserve exclusions until evidence supports changing them.

## Integrate evidence and source safely

- Record the public base/head commit and inspected source when comparing a port handoff to this repository.
  A deployed EXE/DLL hash, local working-tree change or report is not a published source commit.
- Compare actual implementations before importing local fixes. Extract reusable mechanisms; keep game data and
  per-game hooks outside shared defaults. Do not copy an entire game's working tree over this base.
- Read the latest applicable handoff section before historical snapshots. Retain the stated validation limits and
  distinguish source present, adapter installed, path exercised, and result visually/audibly accepted.
- Do not restart historical controllers, deploy game builds, change user saves/settings, or delete captures merely
  because a handoff describes earlier operations. Those require the current task's scope and authorization.
- Run focused tests and required patch/API consistency checks for code changes. Keep the distributable upstream
  patch consistent with vendored source. Documentation-only edits do not require repeating unrelated game tests.

Keep private port handoffs, local paths, assets and unpublished evidence out of public issues or documentation
unless the user explicitly authorizes that disclosure.

See `docs/REUSABLE_BASE.md` and `docs/GAME_PORTING.md` for the current portability audit and porting boundary.

# Dawn of Dreams reference runner

This is a game-specific example, not a runnable game or a universal set of defaults. It needs the port's generated
functions, registration table and auxiliary files. Addresses/ABIs remain confined to this example and its profiles.

For the PR #8 migration, see [the concrete adaptation and feature comparison](../../docs/DOD_RUNNER_PR8_ADAPTATION.md).
`render_profile.h` preserves this runner's GameIndex choices and provides explicit optional particle, static-page
and HD-CG-scaling configuration. Enhancements require an independently confirmed SLPM_662.75 assertion and matching
CRC32 of that verified ELF; no official release fingerprint is invented. The blank `pr8_adaptation.ini` template
is not loaded automatically and enables nothing. The existing low-level particle INI also needs confirmation in
this adapted runner. Force-off remains `PS2_PARTICLE_EDGES=0`.

`prepare_texture_pack.py` stages the public reference DDS exclusions only into a caller-selected, independently
qualified DoD_HD_CN pack. Dry run is default; applying never overwrites a different manifest or modifies textures.
Old GIF/cadence/aspect fragments in `gs_legacy/` remain uninstalled and need separate version-guarded integration
and game validation. This update does not claim visual parity or game acceptance.

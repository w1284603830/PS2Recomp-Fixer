# A reusable base for every port

The project target is a game-independent PS2 recompilation base that anyone can reuse. Shared hardware behavior,
host services and tools form the base; a game's addresses, library ABI, rendering profile and assets are explicit
inputs supplied by its port. A reference game's successful tests define evidence for those cases and versions.

## Integration boundary

| Layer | Shared responsibility | Supplied by each port |
|---|---|---|
| EE/IOP/VU/GS hardware | Instruction, scheduling, DMA, register and memory behavior | Recompiled functions, overlay identities and supported revisions |
| Host services | Audio output, window/presentation, input and diagnostics | File locations and user-selected configuration |
| SDK/library HLE | Adapter mechanisms and identified calling contracts | Verified ABI, table addresses, callback layout and native fallback selection |
| Rendering workarounds | Explicit opt-in, profile lifecycle and draw diagnostics | HPO/grid relationship, material/atlas identification, depth exclusions and validation |
| Game content | Loading/registration interfaces | ISO-derived data, movies, fonts, textures, cheats and gameplay hooks |

## Particle edges require independent qualification for every game

This applies to all games, including Dawn of Dreams, Onimusha 3 and later ports. A validated profile can retain its
tested settings, while another game or revision supplies its own independently qualified profile. Do not treat the
first reference game as an exception to the rule or its constants as defaults for all users.

The existing `DoDNativeParticleEdges` changes sprite coverage and texture sampling. Its `Z >= 0xFF00` HUD exclusion
and HPO5 (`NativeWTexOffset`) prerequisite come from a particular game's rendering conventions. They are not
universal PS2 definitions. An implementation must preserve the relationship between the selected HPO mode and the
vertex/texture coordinate transform; simply accepting HPO4 is not sufficient evidence of correctness.

At audited public commit `311f718106ec88450a002175a9eafbc57978f26b`, the legacy renderer still has the HPO5 gate
and no independent per-game opt-in. The separate Issue #3 implementation now defaults off, matches explicit game,
revision and profile IDs, and clears selection/capture state at GS session boundaries. Its optional DoD policy is
isolated; no Onimusha 3 policy is supplied. See [PARTICLE_EDGE_PROFILES.md](PARTICLE_EDGE_PROFILES.md). The
`PS2_PARTICLE_EDGES=0` switch (legacy `DOD_PARTICLE_EDGES=0`) remains a force-off control. This is configuration
isolation, not evidence that other games had visible defects or that their visuals have passed.

## Public-source portability audit, 2026-11-02

The findings below concern public commit `311f718106ec88450a002175a9eafbc57978f26b`. A local implementation,
binary hash or deployment report must be source-compared before it can be treated as a change in this repository.

| Item | Public-source finding | Required boundary |
|---|---|---|
| Rounded/saturating SPU2 FIR and sinc host output | Present after `6f36851` and merged audio follow-up; focused SSE/FIR and resampler tests exist | Keep shared mechanisms and their documented test limits |
| IOP sticky scheduling | Present in `iop_emulator.cpp` and `iop_kernel.cpp` | Compare implementations before attributing a local result to a missing base fix |
| Per-game GS settings | `Bridge.cpp` accepts explicit `gs_options` and no longer installs fixed DoD GameDB settings | Each runner supplies its own settings; no game's HPO choice is a global default |
| Particle coverage/atlas adjustment | `GSRendererHW.cpp` contains legacy HPO5 qualification and the fixed HUD threshold | Require an explicit independently qualified profile for every game/revision |
| IPU HLE initialization | `IPU.cpp` still has legacy IQ/VQ/helper address defaults | Remaining portability gap: require explicit validated per-game configuration before binding this HLE path |
| MPEG non-DoD HLE path | `MPEG.cpp` still contains fixed globals/callback layouts in its alternative path | Remaining portability gap: identify and configure a supported ABI; "non-DoD" does not mean every game |

This is a source-boundary audit, not evidence of additional game/renderer validation. Private port handoffs,
machine paths, assets and unpublished validation details must remain outside public documentation unless authorized.

## Acceptance record for a candidate shared fix

Record the source/base revision, mechanism versus profile data, exact path activation, focused hardware/unit tests,
directly affected game regression, counterexamples and unresolved limits. Keep shared code, profile selection and
deployed binaries consistent. Update documentation and upstream patches alongside the actual code change.

## Address portability implementation, 2026-11-03

The address changes in this branch close the IPU/MPEG gaps identified above and also remove the inherited font and
host-loop game addresses. Shared hardware register addresses and explicitly supported ABI field offsets remain
constants; game locations are supplied by profiles or the reference runner.

- IPU initialization requires aligned, bounded per-game tables. Missing helpers use direct DMA register setup.
- MPEG selects a supported ABI explicitly, with executable instruction guards and per-game callbacks/globals.
  There is no unknown-game fallback to a legacy layout. HD entries, movie flags and the FIELD workaround are opt-in.
- SCEE font HLE requires a bounded font table, instruction guard and per-game GP offsets; invalid IDs are rejected.
- GParam uses strict address parsing and RAM validation. Capture metadata comes from an optional runner callback.
- ELF loading clears the preceding profile; runners configure the selected game after a successful load.

Headless coverage includes production IPU/MPEG/font translation units, the actual generic runner and shared host
timing/options helpers compiled with GCC and sanitizers. The ordered upstream patch series reproduces the vendored
source byte for byte. This is not a new Windows/MSVC build or evidence of game/codec/font-rendering acceptance. Existing runners
must follow the migration instructions in [GAME_PORTING.md](GAME_PORTING.md#5-address-configuration-and-migration).

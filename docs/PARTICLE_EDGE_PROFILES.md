# Particle-edge profiles

The shared GS defaults to **no particle-edge workaround**, including at HPO5. A runner must explicitly assert the
game, revision and reviewed profile together in `gs_options`:

```ini
gs_options=game_id=onimusha-dawn-of-dreams;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1;half_pixel_offset=5
```

This is the optional [Dawn of Dreams reference configuration](../examples/dod_runner/particle_edges.ini), not a generic
preset. Neither that file nor a matching HPO automatically enables anything. Unknown profiles, missing/mismatched
identity, empty values and duplicate identity/profile keys fail closed. Option order does not matter; surrounding
whitespace is trimmed and IDs are case sensitive. Unrelated image options retain their existing parser/precedence.
`particle_edges_profile=off` explicitly selects no workaround. Remove the selected value before supplying another;
duplicate keys disable the selection instead of silently retaining an earlier game.

`game_id` and `game_revision` are the runner's explicit assertions, **not ELF auto-detection or hash authentication**.
The port must bind them to its verified ELF/revision and record the actual source, EXE/DLL and content identities.
The supplied revision label is the public reference runner's `SLPM_662.75`; this does not certify every executable
sharing a serial, another disc/revision or the whole game. The generic and reference runners remain default off.

## Reference policy and other games

[`ParticleEdgeProfiles/DoD_SLPM66275.h`](../third_party/src/pcsx2gs/pcsx2/pcsx2/GS/ParticleEdgeProfiles/DoD_SLPM66275.h)
preserves the old public renderer parameters from main `05807db13681af24443e6514602b028fcd622e9e`:

| Part | Explicit reference policy |
|---|---|
| Coordinates | Native first/last sample grid, upscale only, HPO5 / NativeWTexOffset |
| Draws | Sprite pairs; exclude texture/channel shuffles; require a native texture, bilinear filtering and a real depth test |
| HUD | Exclude vertices with Z >= `0xFF00`; this is a reference game's convention |
| STQ | Both vertices use the same finite, nonzero Q |
| Atlas sampling | Half a texel inset; with mip filtering use the coarsest read level, capped at level 6 |
| Render targets | Preserve masks, blur and postprocessing by excluding render-target texture sources |

The profile does not invent a new texture-address whitelist or relax old draw exclusions. Its preserved parameters
and numeric compatibility tests are not new visual acceptance. An incomplete sprite pair is rejected before mutation.

**Onimusha 3 has no supplied profile and cannot inherit this one.** Its own game/revision must have its own reviewed
profile, draw qualification and evidence before enabling the workaround. This applies to every game and revision.
To support another port, add an independent immutable profile to `GS/ParticleEdgeProfiles/` and explicitly register
it in `ParticleEdges::FindProfile`. Give it distinct game/revision/profile IDs, qualify every rule and add tests.
The implemented native-grid transform requires the HPO5 relationship, upscaling and uniform STQ Q. A profile requesting
another HPO/transform fails as unsupported; implement and validate the different coordinate relationship first.

## Lifetime, force-off and evidence

GS Open starts from an empty selection. Close, invalid/failed Open and subsequent game opens clear the profile,
environment snapshot, A/B frame marker, diagnostic draw state and queued particle capture requests. A duplicate Open
while a game is active is rejected without altering that active game. All selection/draw access stays on the GS API
thread; asynchronous capture requests are cleared under their existing mutex. Do not hot-swap a profile during draws.

`PS2_PARTICLE_EDGES=0` always disables a selected workaround; `DOD_PARTICLE_EDGES=0` is its legacy alias. When both
exist the `PS2_` value wins, preserving the runtime's established precedence. Environment is sampled afresh at each
Open. A value of `1` never selects a profile or bypasses its identity/draw guards. Rebuild the GS DLL; an older DLL
does not implement this gate and may silently ignore the new option keys. Keep tested host/DLL pairs together.

Startup reports selection/rejection and identity. The GS draw table appends game/revision/profile IDs and reports
profile, environment, draw and vertex exclusions separately; only `applied` means the transform actually ran.
The runtime capture note defers to those decisions instead of assuming "on". A/B records a baseline then a forced-off
frame; an unconfigured baseline stays off. Adjacent frames may differ in animation/RT state and are not frozen replay.

Run `python -m unittest discover -s tests -p test_particle_edges.py -v`. The tests compile production policy headers,
extracted renderer gate/transform and Bridge Open/Close/reset bodies against recording host services. Synthetic
vertices, state switches, forced-off behavior and a frozen public legacy function need no commercial game resources.
They do not compile the complete Windows GS DLL, present a frame or validate GPU behavior. See
[the review record](PARTICLE_EDGE_REVIEW_2026-11-04.md) for separately reported checks and limits.

Real port acceptance still needs frozen-state native/upscaled comparisons of affected particles, HUD/text,
atlas/mip sampling, alpha/depth/occlusion and render-target/mirror/postprocessing paths. Verify the tested draws
actually ran the candidate, and follow [REGRESSION_TESTING.md](REGRESSION_TESTING.md). No current evidence establishes
that another game had visible particle defects, or that this change fixes its visuals.

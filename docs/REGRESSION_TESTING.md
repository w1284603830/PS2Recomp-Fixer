# Complete regression and evidence-based fixes

[繁體中文](REGRESSION_TESTING.zh-TW.md) · [Porting/configuration](GAME_PORTING.md) · [Historical builds](../BUILD_VERIFICATION.md)

This procedure distils the Dawn of Dreams and Onimusha 3 handoff methods into a plan each game/revision must qualify.
It does not claim either game has completed every check. Compilation, static entry coverage, catalogue sweeps,
natural gameplay and visual/audio acceptance are separate evidence. Record original and enhanced modes separately.

## 1. Freeze the reference and candidate

Record source commits, game/region/revision and ELF/image identity, hashes of the EXE and GS DLL actually launched,
toolchain/dependency versions, profile, resolution/scale/aspect, GS/VU backends and original/enhanced mode. Use the
same starting save copy, story state and inputs confirmed consumed by the game. Preserve the user's original saves
and settings. A build-folder binary is not proof that the same binary was launched.

Use a separate candidate directory and verify the EXE, DLLs, resources and settings belong to the intended build.
Check the mounted image and enabled profile after restart. Configure game addresses/ABIs after successful `loadELF`;
see the porting guide. Keep source, built, deployed and accepted versions distinct.

## 2. Check the base and generated entries first

From the repository root:

```sh
python -m unittest discover -s tests -v
```

For the Windows build and bundled suites:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File fetch_deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1
```

Fetch dependencies when setting up or when missing. Inspect individual results: `-NoTests`, a successful script exit
or an unchanged failure count cannot establish acceptance. The build script directly invokes the main, IOP and
individual GS cache cases; a standalone CTest run finding zero cases is not a pass. Match historical exceptions by
test name and current reasoning in `BUILD_VERIFICATION.md`, never by a fixed allowed failure count. Old Windows
results do not validate newly changed source.

After function-map or overlay changes, generate into a new directory, preserve runner/handwritten hooks, use the
repository's preserving/incremental sync tools, then merge the port's registrations and overlays. Check main ELF,
overlay, cross-overlay direct calls, indirect callbacks, loading/relocation and missing-target reports after every
regeneration. Onimusha 3's `entry_coverage.py`, sweep driver and runner generator are **port-local tools, not shipped
here**. Other ports must supply equivalents. Zero missing entries under a static heuristic does not prove dynamic
callback coverage or completion of the game.

## 3. Build a matrix from natural progression

Start with cold boot → memory card/title → menus → new game/load → scene → movement → combat → scene transition →
menus. Confirm input consumption and actual state/action changes before expanding the catalogue. Keep a result for
each room, event, movie and save case. Full acceptance includes chapters, bosses, cutscenes, endings and applicable
New Game Plus, rather than only the opening or sampled rooms.

| Path | Required coverage | Evidence to examine |
|---|---|---|
| EE/overlays/story | Cold boot, native load, chronological room transitions, chapters/bosses, individual events and natural chains | Missing targets, relocation, start/end transitions, actors/cameras; forced room entry is not natural progression |
| GS/VIF/VU | Native/upscaled, 4:3/16:9, window/fullscreen, HUD/fonts/menus, particles, depth/blending, RT/mirror/postprocessing | Matched-state comparison; prove relevant draws exercised the candidate and UI exclusions remain correct |
| GS bridge | Registers, VRAM, transfers/readbacks, SIGNAL/FINISH/LABEL, presentation order and timing | First divergent packet/state, not only final screenshots or average FPS |
| Audio/SPU2 | Music/effects/voice, both cores, reverb, DMA/AutoDMA, cold/warm loading and long playback | Channels/gain/clocks, underruns/drops, synchronisation and listening; one clean counter is not a game-wide guarantee |
| IOP/SIF/disc | Module loading, RPC/DMA/interrupts, reads/EOF, scene and disc transitions | Guest progress and audio clocks; unlimited clock advance or skipped callbacks must not mask stalls |
| IPU/movies/stills | Complete playback, early/late skip, consecutive movies, return to gameplay, subtitles/audio, startup logos/warnings | Verify completion and return; test HD on/off and original fallback when replacements are absent |
| Saves/settings/input | Copy-card creation/save, exit/restart/native load, progress/equipment, dates/subtitles, menu open/close, keyboard/pad | RAM injection is not a save/load test; do not package test saves |
| Performance/stability | Normal user settings, cold/warm loading, long scene/combat sequences | Separate guest/host/GS rates, frame pacing, I/O stalls and instrumentation load |

### Dawn of Dreams reminders

Cover movement, combat, menus, transitions and chapter events through natural play before extending room/movie
sweeps. Test both discs, actual switching and subsequent reads/movies/story return. Qualify merged-image mode
separately if supported. Previous room/movie evidence cannot stand in for untested chapters, bosses, saves, endings
or New Game Plus. Original assets, HD video and widescreen configurations have separate results.

### Onimusha 3 reminders

Use the revision's room/movie/event inventory for sweeps, complete movies, individual events and chains, followed by
chronological progression with real story state. Some scenes inherit the preceding overlay; later events depend on
actors, cameras and flags established earlier. Forced entry from the title or a standalone late event may create an
invalid callback. Reproduce from an appropriate save or preceding scene before assigning a cause; do not replace a
null callback with fake success. Actually trigger the character/weapon/equipment/spell/effect being claimed: installed
resources or an ordinary attack do not validate every action or asset. Qualify each revision and mode independently.

The games supply methods, not reusable room IDs, addresses, script names, concurrency limits or expected durations.
Particle corrections need an independently qualified profile for **every** game/revision, including both references.

## 4. Turn failures into justified fixes

1. **Reproduce and classify.** Freeze source, binaries, configuration, save and consumed input. Separate a real defect,
   unexercised case, missing story prerequisites, faulty judgement and resource contention. Use one instance for
   audio/performance/timing comparisons; rerun contention-suspect failures under low load before changing game code.
   Preserve unresolved or nonreproduced failures as such.
2. **Find the first divergence.** Align a native reference/validated baseline and recompiled candidate at the same
   state. Compare EE → VIF/VU → GIF → GS registers, memory, packets, draws and timing. A CPU rasterizer is an auxiliary
   reference for paths it supports, not a complete GS oracle. Record the first differing PC/RA/overlay and input.
3. **Check content and instrumentation.** Verify an audio spike/silence against the actual source, codec revision and
   matching decode/playback reference; disagreement with one decoder alone is insufficient. Check story state and
   resource loads before diagnosing missing graphics. Installed assets, an enabled adapter, an exercised path and
   human acceptance are separate claims. Pixel comparisons require matching frame/field/state; adjacent-frame A/B
   can include animation and RT differences. Use frozen replay when necessary.
4. **Fix the owning layer.** Support shared hardware/mechanism changes with a minimal reproduction and hardware or
   upstream evidence. Keep addresses, SDK ABIs, HPO/particle filters, atlas/subtitle/content rules in game profiles.
   Do not hide a failure by skipping draws/events, faking callback success or globalising a test exception.
5. **Add a meaningful regression.** Cover the failure and directly related boundaries: wrong revision, address
   ranges, overlay transitions, callback completion, profile reset and original/enhanced mode changes. Assert
   observable behaviour rather than copying implementation conditions into tests.
6. **Rebuild one coherent candidate and retest.** Rebuild affected runtime/runner/GS components, verify launched
   hashes, rerun the failing case, adjacent paths and relevant base tests. Repeat entry checks after generation
   changes. Use targeted regression during repair; run the full matrix appropriate to the change/release candidate
   at acceptance, without repeatedly rerunning unaffected completed catalogues for every local edit.
7. **Close the evidence chain.** Inspect required visuals, continuous playback, controls and audio; record unresolved
   limits and untested scope. Accept only the tested candidate. After deployment, check its real entry point and
   version; build output alone does not establish deployment.

## 5. Keep an auditable case record

| Field | Record |
|---|---|
| Scope | Game/revision, mode, room/event/movie/save case and expected behaviour |
| Reference/candidate | Commits, EXE/DLL hashes, image identity, profile, tool versions and launch arguments |
| Reproduction | Starting save/state, consumed inputs, natural entry versus forced hook, load conditions |
| Exercised path | Entry/overlay, relevant draw or DMA/callback, audio counters/clocks, necessary captures/logs |
| Result | `PASS`, `FAIL`, `BLOCKED`, `NOT_RUN`; give a reason for not applicable |
| Acceptance limits | Automated and visual/listening results separately; fix and retest version |

An untriggered action, empty draw, zero tests, incomplete movie, invalid prerequisites or absent human review is not
a pass for that criterion. Retain failures and their fix/retest links; do not discard failures to obtain green totals.
If only a judge changed, identify which original run was reclassified. Keep necessary version manifests, summaries
and permitted diagnostic evidence; do not upload game data, derived assets or saves into the public repository.
A test plan is not an executed run, and historical handoff controllers, schedules, cleanup or deployments are not
automatically restarted by this procedure.

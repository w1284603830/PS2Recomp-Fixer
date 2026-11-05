# Neutral host, rendering and diagnostic boundaries

This update starts from public main `cc3b5e17f43254db8a06c829172b14b7014e9d52` (tree
`c6f4eef9dbb19a549ef269daf9775cf4b0a882ae`). Recent port changes were compared with actual source; only reusable
mechanisms were extracted. Every game/revision still owns its hooks, ABI, assets and acceptance results.

## Migration

| Area | Shared behavior | Port action |
|---|---|---|
| Host identity | Empty pack namespace and `PS2Recomp port` title | Set `texture_dir` and optional `game_title` in GS options; the pack directory name is a namespace, not authenticated disc identity. |
| Texture exclusions | No compiled texture blacklist | Optional `<texture_dir>/texture_exclusions.txt`: full basenames including extension, one per line. Case insensitive, BOM/whitespace/comments accepted, at most 1 MiB / 4,096 distinct entries / 240 bytes each. Paths, traversal and NUL rejected. Absence/reload/session change clears selection. |
| Aspect ratio | Configured output aspect; `widescreen=1` selects 16:9 | Supply and qualify the game's projection and content policy. Shared code no longer guesses world/menu/CG from framebuffer banks, depth or a full-screen sprite. |
| GIF / scanout | Preserve incoming XYOFFSET and DISPFB values | Old reference-game half-line and CG bank/cadence rewriting lives under `examples/dod_runner/gs_legacy/`. It has no shared call site. A port needing it must implement an identity-guarded adapter and test both GS paths. |
| HD static pages | Disabled until a format is selected | Explicit `static_page_format=page-rgba-512x448-x3-v1;static_page_dir=<pack>`. No executable-relative `ui_hd` auto-load. With a selected format, `PS2_UI_HD_DIR` / legacy `DOD_UI_HD_DIR` can override the directory. |
| HD CG | Explicit HCG1 packet ABI; full native RGB proof before an upload | Supported native frame dimensions remain 512×288/336 with half-height previews and 1920×1080 HD output. Other layouts need a new adapter. No fixed TBP0 whitelist; validate format, stride, masks, aperture, alpha and subtitles in the port. |
| HD scaling | Keep configured scale and NativeScaling | Optional qualified `hd_frame_min_scale=1..8` and `hd_frame_disable_native_scaling=1`. No automatic 7× or automatic NativeScaling override. The existing scoped restoration remains. |
| Feedback / clamped sampling | Existing behavior by default | Independently qualify `feedback_copy_hpo_guard=1` and `clamp_footprint=1`. These explicitly selected port policies are separate from particle profiles and do not enable them. |
| VU library | Normal clamp mode 1, IbitHack off | `KzvuConfig` callers explicitly select their game's settings. Killzone reference tests retain their own clamp-0/IbitHack-on configuration. |
| Diagnostic scripts | General tools accept caller-owned data | Eleven scripts with fixed reference ELF ranges/counters moved to `examples/dod_runner/tools/`; see [tools/README.md](../tools/README.md). They remain game-specific examples. |

Texture exclusion reference data is in `examples/dod_runner/texture_exclusions.txt`; copy it only into that game's
qualified pack. `.dds` and `.png` entries are distinct so a repaired PNG remains eligible. The HD static-page adapter
retains `pages.tsv` columns `id, preview PNG, HD PNG, top, full/even/odd`, tab separated; preview is 512×448 and HD is
1536×1344. It proves native RGBA for each dirty rectangle and source alpha for the HD mapping. These dimensions are
an asset contract, never game detection. UI predecode originated as an optimization for **HD texture replacement
porting**; each port supplies the manifest and memory budget described in [HOST_TIMING.md](HOST_TIMING.md).

GS Open/Close and failed/invalid Open reset port options, pack/title, fullscreen/aspect choices and capture state.
A rejected duplicate Open preserves the running session. Old DLLs silently ignore new keys: deploy a matching tested
host/DLL pair. There is no hot swap of renderer options during draws.

## Fresh input and presentation

Host event processing now updates menu capture, publishes fresh pad input, then enters the host idle wait. With an
asynchronous GS the pump waits 1 ms even with `fps_limit=60`; synchronous/CPU paths retain their pacing selection.
EE/IOP/SPU clocks, game logic, animation and attack windows are unchanged. `PS2_GS_MAX_FIELDS=1` selects a shorter
producer queue; absent, zero, malformed and out-of-range values keep 2. This is a host queue bound, not a measured
physical button-to-screen latency. No zero-queue or 250 µs experimental polling preset is installed.

A validated native pad adapter can use `ps2_input_readers.h`:

1. After a successful `loadELF`, register a reader. Up to eight independent readers are supported; zero means full.
2. At a validated native read boundary, call `readLatest(token, buffer, 32)`. Invalid tokens/buffers fail without
   acknowledging input. Each reader acknowledges its own retained digital short presses; SIO2/HLE consumption is
   independent. Registration begins with no historical pending presses; current held state is still readable.
3. Validate the game's native function identity, output pointer, length, port/slot, packet format and mode before
   copying selected fields. The 32-byte host snapshot is not a replacement libpad ABI. Pressure values represent
   the latest sample; a game's pressure protocol/pulse mapping needs its own adapter and tests.
4. Unregister when the adapter closes. ELF/session reset invalidates tokens and discards prior input. Menu capture
   clears retained input for all consumers and releases buttons/sticks; device-source changes discard stale edges.

No Onimusha 3 native addresses, packet offsets, pressure reconstruction or gameplay hooks are supplied.
A port-owned integration sketch is in [host_policy_example.h](../examples/generic_runner/host_policy_example.h);
it is not automatically included or configured.

The default scheduler presents at VBlankStart. A port that has qualified late host submission may call:

```cpp
// After loadELF and before run(), on the EE configuration/execution thread:
runtime.eeScheduler().configurePresentation(ps2host::PresentationPhase::End);
// Wire these calls to the port's validated movie lifecycle on that same thread:
runtime.eeScheduler().setPresentationMovieActive(true);  // use Start for the next field
runtime.eeScheduler().setPresentationMovieActive(false); // resume configured phase
```

Include `runtime/ee_scheduler.h` when calling these methods. The per-field decision latches at Start:
a movie begin/end occurring mid-field cannot double-submit or skip the field's submission. `run()` resets pending
field/movie state while preserving the post-load phase; loading another ELF restores the default Start profile.
Cover native, replacement and fallback movies, failed starts, aborts and end-of-stream. A caller that cannot establish
movie state should retain Start. No movie flag or Onimusha 3 lifecycle is guessed from HPO or draw state.

## Captures and diagnosis

`dump_alpha=1` enables GS alpha evidence. PNG `RGB_A_PNG` now emits opaque RGB plus `<stem>_alpha.png` containing the
source alpha as grayscale; both file writes must succeed. RGBA, indexed masks, channel order and padded row pitch
are separately tested. A filename alone does not establish which target or draw was captured.

The A/B hotkey requests eight draw frames: four baseline then four forced-off, with an initial presentation image
for alignment. Direct API A/B requests are bounded to 4..8 draw frames. Inspect draw counts, source/target identities,
selected profile, `applied`/skip reasons and the actual ROI in both halves. Empty/alternating fields and different
buffers can make a single pair misleading. An off/unqualified baseline stays off. Adjacent frames advance animation
and render-target contents; use frozen-state replay for effect acceptance. A/B force-off ends at capture end and is
cleared on session changes. Optional `PS2_CG_GS_TRACE=first:count` observes caller metadata/GIF bytes without modifying
registers or enforcing cadence.

`PS2_PAD_BOOT_SEQUENCE=START,CIRCLE` is an example of explicitly supplied boot navigation, not a default. The old
`DOD_TEST_AUTOSTART` / `PS2_TEST_AUTOSTART` variable alone selects no buttons. Its numeric value now supplies an
explicit delay in seconds; no special `1 = 40 seconds` behavior remains. Boot steps repeat every 1.5 s with a 150 ms
press until the runner publishes its test epoch. `PS2_PAD_SCRIPT_FRAMES` needs an explicit bounded RAM word and
`PS2_PAD_SCRIPT_HZ`; absence/invalid frequency uses host seconds. Session/epoch changes reset frame baselines and logs.
Do not infer another game's counters, boot sequence or frame frequency from a reference runner.

## Test and acceptance procedure

Run in an isolated build environment. This change's work did not compile on the user's computer.

```sh
python -m unittest discover -s tests -p test_neutral_base.py -v
python -m unittest discover -s tests -p test_particle_edges.py -v
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
```

The suites compile production pad code, policy/asset headers, capture/identity functions and Bridge/renderer bodies
against recording services. Synthetic concurrency, lifecycle and content-match tests need no commercial assets.
The patch round-trip reproduces vendored PS2Recomp byte for byte. GCC syntax checks of full Linux scheduler/backend
translation units complement those fixtures. `.github/workflows/neutral-base.yml` adds headless CI and a complete
Windows GS DLL compile on a GitHub-hosted runner; its actual run status must be checked separately.

Then follow [REGRESSION_TESTING.md](REGRESSION_TESTING.md): freeze source/profile/assets/EXE/DLL identities, compare
original/enhanced modes, record consumed input and native states, find the first divergence along EE → VIF/VU → GIF → GS,
and repair the layer where it begins. Cover cold/warm transitions, particles/HUD/text, alpha/depth/RT effects, movie
start/seek/end/fallback, input/menu/device changes and sustained audio/A/V sync. Repeat directly affected cases after
a minimal repair, then the full release matrix. Record static review, synthetic/manual simulation, actual compile
and real-game visual/listening acceptance separately. No software trace proves USB/display latency or all-GPU/all-game
compatibility. See [the review record](NEUTRAL_BASE_REVIEW_2026-11-05.md).

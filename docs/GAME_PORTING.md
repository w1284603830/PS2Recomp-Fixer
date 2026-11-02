# Porting a game with this base

This repository is the reusable part of a PS2 static-recompilation port. It contains no game. Shared hardware and host services are reusable; game addresses, supported library ABIs and
rendering workarounds need an explicit profile qualified for each game/revision. A reference runner is not a universal
set of defaults. See the [reusable-base contract](REUSABLE_BASE.md).

## 1. Pieces

| You provide | Where |
|---|---|
| The generated functions of your game (`ps2_recomp` output) | `third_party/src/PS2Recomp/ps2xRuntime/src/runner/` (git-ignored upstream, `git add -f` your own) |
| `register_functions.cpp` and a `main.cpp` | the same folder; start from [`examples/generic_runner/main.cpp`](../examples/generic_runner/main.cpp) |
| `game.ini` next to the executable | settings of the runner (keys listed at the top of its `main.cpp`) |
| `cheats.ini` (optional) | RAM cheats for the F2 panel, [CHEATS.md](CHEATS.md) |

[`examples/dod_runner/`](../examples/dod_runner) is the first port's entry point (two discs, HD movies, widescreen,
dozens of test hooks): a reference for what a finished runner can do, not a template to copy whole.

## 2. Game-specific values and where they go

| Value | How to pass it | Notes |
|---|---|---|
| Boot ELF, disc image, window title/size | `game.ini` `elf`, `iso`, `title`, `window` | |
| PCSX2 per-game GS fixes (`GameIndex.yaml`: blending level, half pixel offset, native scaling, bilinear upscale...) | `gs_options`, e.g. `blending=2;half_pixel_offset=5;native_scaling=2;bilinear_upscale=2` | Nothing is applied by default. Keys: `Bridge.cpp` `ApplyOptions`. Look your game up in PCSX2's `GameIndex.yaml`. |
| Guest address of libgraph's GParam record (what the game's `sceGsGetGParam` returns) | `game.ini` `gparam`, `PS2_GS_GPARAM=0x...`, or `ps2_stubs::setGsGParamAddress()` | Without it `sceGsSyncV` uses the HLE's own record. Needed when the game's libgraph runs natively and updates its own record. |
| libipu tables (IQ, VQ, optional D4_CHCR helper) | `game.ini` `ipu_iq`, `ipu_vq`, `ipu_d4_chcr`, or `ps2_stubs::dodConfigureIpuInit()` | No default addresses; unconfigured/invalid HLE init returns -1 before touching guest state. |
| MPEG library ABI and globals | `ps2_stubs::configureMpegGameProfile()` after `loadELF` | Explicit supported ABI, instruction guards, callbacks and globals. Unknown/mismatched profiles never fall through to another game's layout. |
| SCEE font HLE globals | `ps2_stubs::configureFontGameProfile()` after `loadELF` | Explicit table/count, instruction guard and GP offsets. The generic runner leaves this game-specific adapter disabled. |
| Capture metadata | `runtime.setCaptureMetadataProvider()` after `loadELF` | Optional callback; the shared host loop reads no game's scene/frame addresses. |
| RAM cheats | `cheats.ini` | |
| Diagnostics | `PS2_*` environment variables (below) | |

## 3. Environment variables

The opt-in switches and diagnostics are environment variables. Their original names start with `DOD_` (the first port);
every one of them can be written with the `PS2_` prefix instead: `PS2_PAD_TRACE=1` is `DOD_PAD_TRACE=1`. The runtime
copies `PS2_*` to the `DOD_*` names at start (`ps2_env.cpp`); when both are set the `PS2_` one wins.

Ones with game-specific content now take that content from you instead of fixing it:

| Variable | Meaning |
|---|---|
| `PS2_RAM_DUMP=<dir>` | every 300 fields, EE RAM to `<dir>\tick_<n>.bin`; `PS2_RAM_DUMP_RANGE=0xFIRST-0xLAST` picks the part (default: all 32 MB) |
| `PS2_VU0_TRACE=<n>`, `PS2_VU0_TRACE_RANGE=0xFIRST-0xLAST` | the first n calls of each function in the range (the game's libvu0); nothing is traced without a range |
| `PS2_PAD_TRACE=1`, `PS2_PAD_TRACE_ADDRS=a0,...,a10` | the host pad trace always works; the guest-side libpad trace also needs the 11 guest addresses of the game's libpad variables (order in `EeScheduler.cpp`) |
| `PS2_GS_GPARAM=0x...` | as above |
| `PS2_AUDIO_LINEAR=1` | host audio output back to linear interpolation; the default is a 64-tap windowed-sinc resampler (`ps2_output_resampler.h`, fixed 0.646 ms delay, same queue control and game clock) |
| `PS2_SPU2_REVERB_STEMS=<file>`, `PS2_SPU2_REVERB_CORE=0` | dump the reverb input / return of core 1 (or core 0) as raw 4-channel 16-bit 48 kHz |
| `PS2_CHEATS_FILE`, `PS2_CHEATS`, `PS2_CHEAT_MENU_AT` | see [CHEATS.md](CHEATS.md) |

## 4. What is still the first port's

These keep their `dod` names and some of its assumptions; check them before you rely on them for another game:

- `Kernel/Stubs/dod_hd_cg.h`, `MPEG.cpp` HD movie hooks, `gs/dod_cg_cadence.h`, `DoDHdCg.h` (in the GS): HD replacement
  movies with the first port's movie table and container.
- `GSTextureReplacements.cpp` `Dod*` (HD texture packs, room prefetch list): the mechanism is generic (a texture
  directory plus a prefetch list), the file names and the scene variable the runner reports are the port's.
- `DOD_TEST_AUTOSTART`, `DOD_TEST_ROOM`... in `ps2_pad.cpp`: automated test scripts; they wait for a flag
  (`g_dodTestNewGameAt`) that your runner sets from a hook on your game's own function.
- Names such as `dodKzvuInstall`, `dodpcsx2`, `dod_input_settings` are only names: they are the runtime's VU1, GS
  front-end and input code, used by every game.

Keep the `third_party/src/<module>` layout; the sources reference each other through relative paths.

## 5. Address configuration and migration

`loadELF` clears IPU, MPEG, font and GParam configuration and capture metadata, including on a failed load. Configure
the selected game **after** a successful load and before `run`; `run` resets MPEG playback but preserves the chosen
profile. Do not reconfigure profiles while guest execution or callbacks are active. The existing MPEG host state
supports one active runtime/session. Reset installed HD hooks with `resetMpegGameProfile(&runtime)` before changing
that profile; reload on that runtime restores its original functions.

The generic runner accepts unsigned decimal or `0x` addresses; trailing junk, negative values and overflow are errors.
GParam needs a 4-byte-aligned 12-byte record in EE RAM, or zero to select the HLE record. `PS2_GS_GPARAM` and its legacy
alias use the same validation. IPU IQ needs 80 bytes and VQ 32 bytes, both 16-byte aligned; the optional helper needs a
4-byte-aligned instruction. The normal, uncached, KSEG0 and KSEG1 RAM aliases are accepted; MMIO is rejected. Missing
or unregistered D4_CHCR helpers use the direct register setup. These settings affect HLE `sceIpuInit` only; a native
libipu/IPU path does not need them.

MPEG's [`MpegGameProfile.h`](../third_party/src/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MpegGameProfile.h) supports the
existing `Work19A0` and `Legacy118` adapters. Selecting a layout is not proof that another SDK revision matches it.
Supply at least one verified instruction guard and both default callbacks. `Legacy118` additionally needs
`objectGlobal`, `referenceTable`, nine `references` and `resetGlobal`; no old addresses are supplied automatically.
At an unknown/mismatched profile, Create/Init return zero and other HLE entries return -1 without guest writes.
Use recompiled native library functions or implement a supported adapter for other ABIs.

HD movie routing additionally needs a verified GP-relative movie ID offset, flags table/stride/count and all eleven
`hdEntries` in `MpegEntry` order. Omit them to retain original video. Installation requires every original entry to
exist and the instruction guards to match; repeated installation is a no-op. The optional CSR.FIELD workaround is
also default off and needs its own PC/instruction pair. It retains `PS2_MOVIE_FIELD=keep` / `DOD_MOVIE_FIELD=keep`.
[`examples/dod_runner/game_profile.h`](../examples/dod_runner/game_profile.h) holds only the public SLPM_662.75 reference
settings; copy it beside that example runner when installing the runner. Do not use its addresses for another game.

The font adapter in [`Font.h`](../third_party/src/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Font.h) implements a specific
0x24-byte entry layout. Supply its table/count, an executable instruction guard and eight distinct aligned GP offsets
in `FontGlobal` order. Runtime GP arithmetic, revision mismatch and font ID bounds are checked before using the table.
There is no default font profile. A different font library needs its own adapter.

Particle-edge workarounds still require independent qualification for **every** game/revision, including the original
reference game. HPO values, HUD/depth exclusions and atlas rules do not become universal by configuring these addresses.
The separate rendering work is tracked in [issue #3](https://github.com/w1284603830/PS2Recomp-Fixer/issues/3).

Before acceptance or release, follow the [regression and repair guide](REGRESSION_TESTING.md)
([繁體中文](REGRESSION_TESTING.zh-TW.md)). Entry coverage, content sweeps and natural playthrough are distinct gates.

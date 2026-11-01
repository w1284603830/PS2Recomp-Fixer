# Porting a game with this base

This repository is the reusable part of a PS2 static-recompilation port. It contains no game and, since the
generalisation pass, fixes no game-specific value in the GS bridge or the kernel stubs: everything that differs between
games is a setting you pass in, or an explicit hook in your own runner.

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
| libipu tables (IQ, VQ, D4_CHCR helper) for the IPU HLE `sceIpuInit` | `ps2_stubs::dodConfigureIpuInit()` | The defaults in `IPU.cpp` are another game's. Do not rely on the HLE `sceIpuInit` unless you set them. |
| MPEG library ABI | none | The HLE MPEG paths in `MPEG.cpp` check the original instructions of the game's libmpeg (`usesDodMpegAbi`) before they use the first port's layout; other games take the generic path. The HD movie hooks (`dodInstallHdMovieHooks`) are the first port's and are not installed by the generic runner. |
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

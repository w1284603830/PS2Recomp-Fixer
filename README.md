# PS2Recomp-Fixer — a PCSX2-accurate PS2Recomp base

[繁體中文說明](README.zh-TW.md)

A fork of [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) (commit `eb8688b`), the PlayStation 2 static
recompiler, hardened while building a full PC port of a commercial PS2 game (*Shin Onimusha: Dawn of Dreams*: two
discs, 3D, CRI Sofdec/ADX movies, IOP sound drivers; every room and movie run by automated sweeps). About 150 runtime,
recompiler and IOP fixes were made, most of them by following PCSX2's behaviour, and the hardware emulation pieces
the upstream runtime lacked were added.

**This repository contains no game.** It is the reusable base: recompiler, runtime, IOP emulation, PCSX2 GS and VU
modules, tools and tests. To port a game you need your own legally obtained copy of it.

**Project target: a game-independent base anyone can reuse.** Shared mechanisms implement hardware and host
services; every port supplies explicit game/revision profiles for addresses, library ABIs and rendering workarounds.
Particle-edge fixes require independent qualification for every game. Development rules: [AGENTS.md](AGENTS.md);
current boundaries and source audit: [docs/REUSABLE_BASE.md](docs/REUSABLE_BASE.md).

## Highlights

- **PCSX2's hardware GS** (DX11/DX12) as `pcsx2gs.dll`, fed on its own thread; upscaling, FXAA/SSAA, HD texture
  replacement packs with async loading, exclusive/borderless fullscreen
- **PCSX2 microVU** for VU1 (and VU0 micro mode) through `kzvu`, replacing the cycle-budgeted VU1 interpreter
- **SPU2** emulation on the IOP (voices, ADPCM, Gaussian interpolation, envelopes, AutoDMA, reverb) — games that run
  their own sound drivers on the IOP have sound
- **SIO2 + DualShock 2** (pad stack runs unmodified on the IOP), SDL2 gamepads with rumble
- **IPU** (IDEC/BDEC/VDEC/CSC) for libmpeg/Sofdec movies, FFmpeg path for HLE MPEG
- Threaded IOP, IOP critical sections and RPC that behave like the real kernel
- EE kernel fixes: thread priorities, Suspend/Wakeup return values, DI/EI critical sections, masked interrupts,
  NTSC 59.94 Hz VBlank in real time, interrupt handler stacks that no longer overwrite game stacks
- DMA/VIF/GIF fixes: VIF1 UNPACK semantics, chain CALL/RET depth, IMAGE continuation, D_ENABLE, CHCR/STR, SPR chain
- Recompiler fixes for MMI/FPU/VU0 macro semantics, 64-bit branch compares, VU0 random number instructions
- CD/DVD: real ISO9660 LBNs, dual-layer DVD9, disc swapping
- Host: one deadline frame pacer, UTF-8 process code page (runs from any folder name)
- Many opt-in diagnostics (`PS2_*` environment variables; the original `DOD_*` names still work) and offline analysis tools
- `native = ["name@0xADDR"]` recompiler selectors keep a game's own function recompiled when its name matches an
  SDK call the runtime would replace; IOP `thmsgbx` message boxes; MCMAN-style memory card descriptors; libpad rumble
- **Optional, off by default:** an F2 cheat menu driven by your game's own `cheats.ini` and an F3 trainer panel for
  switches your port registers ([below](#optional-cheat-tools-off-by-default)); no game's cheats are shipped
- `tools/new_port.py`: from your disc image to a port folder with the boot ELF, recompiler config, `game.ini` and the
  exact next commands
- Code a game loads at run time (overlays): `ps2_recomp` takes a raw image and its load address (`[overlay]`), and
  the runtime dispatches through whichever registered image's signature is in RAM; off unless a port registers images
  ([docs/GAME_PORTING.md](docs/GAME_PORTING.md) section 6)

Full list with references: [IMPROVEMENTS.md](IMPROVEMENTS.md). Line-by-line PS2Recomp changes are distributed as
[an ordered patch series](patches/README.md) for upstream `eb8688b`: apply the retained base patch, then the shared
maintenance deltas listed there. The checked-in vendored source already contains the complete series.

## Layout

| Path | Contents |
|---|---|
| `third_party/src/PS2Recomp/` | The improved PS2Recomp: `ps2xRecomp` (recompiler), `ps2xAnalyzer`, `ps2xRuntime`, `ps2xIOP`, `ps2xTest` |
| `third_party/src/pcsx2gs/` | PCSX2's GS built as `pcsx2gs.dll` with a C API (`pcsx2-gs/include/pcsx2gs_api.h`) |
| `third_party/src/kzvu/` | PCSX2 microVU as `kzvu.lib` (from KillzoneRecomp) |
| `fetch_deps.ps1` | Downloads the unmodified, version-pinned dependencies (SHA256-checked) |
| `build_all.ps1` | Builds everything and runs the tests |
| `tools/` | Recompilation pipeline scripts (function map fixes, jump tables, generated-code sync, ISO extraction); `tools/diag/` analysis tools |
| `examples/generic_runner/main.cpp` | A game-agnostic host entry driven by `game.ini`: copy it as your `main.cpp` |
| `examples/dod_runner/main.cpp` | The game entry of the port this came from (two discs, HD movies, test hooks), as a reference (not built) |
| `patches/` | Diff against upstream `eb8688b` |
| `docs/GAME_PORTING.md`, `docs/CHEATS.md`, `docs/TRAINER.md` | Where each game-specific value goes; the optional F2 `cheats.ini` menu and F3 trainer |
| `docs/ECOSYSTEM_SURVEY.md` | Survey of related recompilation / native-port projects: lineage, IOP routes, VU, GS |
| `docs/` | Development notes from the original port (Chinese): every patch's cause and verification, build notes, upstream test review |

The sources reference each other through relative paths, so keep the `third_party/src/<module>` layout.

## Requirements

- Windows 10/11 x64, a CPU with SSE4.1 (PCSX2's GS)
- Visual Studio 2022 with *Desktop development with C++* and *C++ CMake tools* (tested with MSVC 19.44, CMake 3.31)
- git and Windows PowerShell 5.1
- Python 3.9+ for validating named test results when running the build tests
- About 2 GB of free disk space for dependencies and a full build

## Build

In PowerShell (not Git Bash: it rewrites `/DWIN32`-style flags into paths):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File fetch_deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1
```

`build_all.ps1` builds `kzvu.lib`, then `pcsx2gs.dll`, then PS2Recomp (`ps2_recomp.exe`, `ps2_analyzer.exe`,
`ps2_runtime.lib`, `ps2EntryRunner.exe`, test programs), and runs the tests. Main-suite failures are checked by suite/name/status against reviewed historical exceptions; empty/incomplete
reports and new failures fail the build. Python 3.9+ is required for this check. Options: `-BuildDir <folder>`,
`-Part kzvu|gs|ps2recomp`, `-NoTests`. Output goes to `build\`.

The `ps2EntryRunner.exe` built here uses upstream's generic `src/main.cpp` and an empty function table: it only proves
that the runtime links. A game needs its own generated code and entry point (below).

## Quick start for a new port

```
python -I tools/new_port.py --iso D:\games\MyGame.iso --out D:\ports\mygame --title "My Game"
```

It reads SYSTEM.CNF, copies and checks the boot ELF, writes `recomp.toml` (with `ps2_analyzer` when it has been
built), `game.ini` for the generic runner and `PORT_STEPS.md` with the remaining commands. The port folder must be
outside this repository; game files are never committed. The manual route is below.

## Optional cheat tools (off by default)

Both are compiled out unless a build asks for them: no F2/F3 handling, no cheat file read, no guest RAM written.

| Tool | Enable | Docs |
|---|---|---|
| F2 cheat menu: RAM cheats from the game's own `cheats.ini` | `-DPS2X_ENABLE_CHEAT_MENU=ON` or `build_all.ps1 -CheatMenu` | [docs/CHEATS.md](docs/CHEATS.md) |
| F3 trainer: a panel for the switches/choices a port registers for its own code hooks (`ps2_trainer.h`) | `-DPS2X_ENABLE_TRAINER=ON` or `build_all.ps1 -Trainer` | [docs/TRAINER.md](docs/TRAINER.md) |

With the trainer off, a port's `ps2trainer::on()` / `choice()` hooks compile to constants that keep the original code.

## Porting a game

1. Extract the boot ELF and IRX modules from your disc image: `python tools/iso_extract.py <iso> <out dir> <max bytes>`
2. Export the function list with Ghidra: `analyzeHeadless … -postScript ExportPS2Functions.java <toml> <csv>`
   (script in `ps2xRecomp/tools/ghidra`)
3. Validate the function map against this game's ELF segments, data/code ranges, indirect calls and switch targets.
   The fixed-range `scan_code_pointers.py`, `fix_function_map.py`, `scan_jump_tables.py` and
   `find_missing_function_starts.py` now live in `examples/dod_runner/tools/`; adapt their reference assumptions
   before using them on another revision. See [tool boundaries](tools/README.md).
4. Identify SDK functions: `ps2_analyzer <elf> analyzer.toml`, then `merge_stubs.py`
5. Recompile: `ps2_recomp <config.toml>`; copy the output into `ps2xRuntime/src/runner/` and
   `ps2xRuntime/include/` (`sync_generated*.py` copy only changed files to avoid full rebuilds)
6. Write the game's `ps2xRuntime/src/runner/main.cpp`: copy `examples/generic_runner/main.cpp` (settings come from
   `game.ini`, see [docs/GAME_PORTING.md](docs/GAME_PORTING.md)), or start from upstream's `src/main.cpp` or
   `examples/dod_runner/main.cpp`. To use this base's additions:
   - `dodKzvuInstall(runtime, "microvu")` — VU1 on PCSX2 microVU (otherwise the built-in VU1 interpreter runs)
   - `dodpcsx2::configure("pcsx2", scale, 0, "gs options")` — PCSX2 GS scale and options (the PCSX2 GS is used by
     default when `pcsx2gs.dll` is present)
   - `runtime.replaceFunction(address, …)` to replace or wrap game functions
7. Pass the game-specific values ([docs/GAME_PORTING.md](docs/GAME_PORTING.md)): PCSX2's per-game GS fixes in
   `gs_options`, and the address of the game's libgraph GParam record (`gparam`, used by `sceGsSyncV`) when its libgraph
   runs natively. Configure IPU/MPEG/font adapters only with verified per-game data, after successful `loadELF`;
   unconfigured adapters no longer inherit another game's addresses. Review remaining rendering-specific assumptions
   in [docs/REUSABLE_BASE.md](docs/REUSABLE_BASE.md).
8. Next to the executable: `pcsx2gs.dll`, `resources\` (from `pcsx2gs/pcsx2-gs/resources`), `SDL2.dll`
   (`third_party/src/SDL2-2.30.9/lib/x64`), the FFmpeg DLLs (copied by the build) and your game files

`docs/DoD_BUILD.md` has the exact commands used for the original port.

The DoD reference runner has a separate [PR #8 adaptation guide](docs/DOD_RUNNER_PR8_ADAPTATION.md), with
explicit revision/ELF confirmation, optional reference settings and uninstalled legacy-feature boundaries.

## Tests

The [neutral-base migration guide](docs/NEUTRAL_BASE_MIGRATION.md) covers fresh input, independent native readers,
opt-in VBlank submission, explicit HD formats, pack-owned exclusions and multi-field alpha/A-B diagnosis.
Game-specific GIF/CG/aspect heuristics and fixed-range analysis scripts now live in the reference example.
See the [review and validation limits](docs/NEUTRAL_BASE_REVIEW_2026-11-05.md); every port still needs its own regression.


Particle-edge workarounds now default off and require matching explicit game, revision and profile IDs; HPO alone
cannot enable them. The optional Dawn of Dreams reference policy is isolated, and Onimusha 3 requires its own profile
and validation. See [PARTICLE_EDGE_PROFILES.md](docs/PARTICLE_EDGE_PROFILES.md) for configuration and evidence limits.

Host pacing/latency controls and pack-owned UI decode preloading are documented in
[HOST_TIMING.md](docs/HOST_TIMING.md). Queue-drain intervals, DXGI overrides and texture lists require per-port
qualification; existing SPU2/reverb precision is retained. Apply the regression guide below after tuning them.
UI predecoding originated as an HD texture replacement porting optimization; its pack and atlas list remain port-owned.

Results of a clean build on 2026-10-27 ([BUILD_VERIFICATION.md](BUILD_VERIFICATION.md)):

| Suite | Result |
|---|---|
| `ps2x_tests` | 421 / 446 pass. The 25 others are known, reviewed differences: the tests expect upstream's behaviour where this base follows the hardware or PCSX2 (list and reasons in BUILD_VERIFICATION.md) |
| IOP tests (5 programs) | all pass |
| GS cache tests | 45 / 45 pass |

These are historical results, not acceptance of later changes or another game. The current headless address-profile
checks are documented in [tests/README.md](tests/README.md); they do not replace a Windows build or game validation.

### Full regression and how to fix failures

Follow [the complete regression guide](docs/REGRESSION_TESTING.md) ([繁體中文](docs/REGRESSION_TESTING.zh-TW.md)),
distilled from the Dawn of Dreams and Onimusha 3 handoffs:

- **Separate evidence levels.** A successful build, an empty runner, zero static missing entries or a room sweep does
  not mean the game passed. Run the headless suite (`python -m unittest discover -s tests -v`) and bundled build
  suites; inspect named failures, not only exit codes/counts. CTest finding zero cases is not a pass.
- **Cover natural progression.** Cold boot → menus → new game/native load → movement/combat → scene transitions;
  extend to all chapters, bosses, cutscenes, save/exit/reload, endings and applicable New Game Plus. Test original and
  enhanced modes separately, including HUD/fonts/particles, audio/clocks, IOP/SIF, movies/stills and normal user settings.
- **Keep per-game requirements.** Dawn of Dreams needs both discs and actual disc-switch return paths, plus merged
  images when supported. Onimusha 3 needs main/overlay entry checks, rooms, individual events and natural chains;
  forced entry may lack an inherited overlay or story state. Neither game's evidence validates another port.
- **Diagnose the first divergence.** Freeze commits, launched EXE/DLL hashes, profile, save and consumed input;
  compare the same native/reference state through EE → VIF/VU → GIF → GS. Validate audio against its actual source
  and codec revision. Adjacent-frame A/B is not frozen-state replay; an applied draw is not visual acceptance.
- **Repair and retest a coherent candidate.** Distinguish hardware, ABI/profile, content and test-driver causes;
  keep game addresses and rendering exceptions in explicit profiles. Add a focused regression, rebuild affected
  components, rerun the failure and adjacent paths, then complete the acceptance matrix for the release candidate.
  Recheck contention-suspect audio/timeouts with one instance before changing game logic.
- **Report limits honestly.** Record `PASS` / `FAIL` / `BLOCKED` / `NOT_RUN`, actual path activation and visual/listening
  review. Preserve unresolved failures and version evidence; untested cases are not passes. Every game's particles
  require independent qualification, including both reference games. Do not ship game data or test saves.


## Author

She39 — the original PC port and this base.

## License

GPL-3.0 (`LICENSE`). Third-party code, origins and licenses: [THIRD_PARTY.md](THIRD_PARTY.md).

Credits: [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp), [PCSX2](https://github.com/PCSX2/pcsx2),
[KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp) (kzvu and several runtime patches),
[urban-reign-recomp](https://github.com/thiagordsfx/urban-reign-recomp) (PCSX2 GS bridge, IPU integration),
AC5-Recomp (IPU), and the other PS2Recomp-based projects whose findings are cited in the source comments.

This project is not affiliated with Sony Interactive Entertainment, Capcom or the PCSX2 team. PlayStation 2 is a
trademark of Sony Interactive Entertainment.

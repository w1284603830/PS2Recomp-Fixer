# Cross-device performance and compatibility for PS2 recompilation ports

[繁體中文](CROSS_DEVICE_OPTIMIZATION.zh-TW.md) · [Porting](GAME_PORTING.md) · [Regression testing](REGRESSION_TESTING.md) ·
[Host timing](HOST_TIMING.md)

Reports of the form "the movies stutter on some PCs", "some PCs cannot play movies", "the game stutters", and
"it does not start on Xeon PCs" collected from ports built on this base. Each item below is game-neutral (a shared
mechanism, host service or build setting) and was implemented and measured in one port (Ultraman Fighting Evolution
Rebirth, SLPS-25529) on one PC (i5-12400F, RTX 3080, Windows 10). The numbers are that port's; measure your own game
before claiming anything. Source changes are tagged `[Fixer]`.

## 0. Measure first: what to look at and how

| Question | Method (tools in `tools/perf/`) |
| --- | --- |
| Which thread limits the game? | `thread_cpu_sample.py <pid> <s> out.json`: CPU % per named thread (`GameThread` = EE, `dod GS`, `dod IOP`) per second, per phase. Total process CPU hides the limiting thread. |
| Where does that thread spend time? | `thread_profile.py <pid> GameThread 40 runner.map out.json` (RIP sampling at 1 kHz, symbolized with the EXE's `/MAP`); for a DLL add `--map pcsx2gs.dll=pcsx2gs.map`. Expect about half the samples in `ntdll` (waiting for the frame). |
| Is it smooth for the player? | Game frames/s (display flips, DISPFB changes) next to presented fields/s. Under CPU contention the fields can stay at 60 while the game drops to half speed. |
| How does it behave on a slower PC? | `cpu_cap.py <pid> --cpus 2,4 --step T:C ...` caps the process to C logical CPUs after the route reaches the content. Run old and new builds side by side on disjoint CPUs; expect several fps of noise. |
| Does it survive other programs running? | Run 1 busy process per logical CPU next to the game and read the game frames/s. |
| Did a change alter game behaviour? | Cross-build determinism: catch-up off, forced seeds, frame-locked input, EE RAM dumps at fixed logic frames. First measure a same-build pair (the noise floor: host-timing counters), then compare builds. |
| Is a decoder change bit-exact? | Hash the bytes delivered to the game at fixed milestones (the IPU prints FNV-1a every 16 MiB with `DOD_PERF=1`) for old and new paths. |
| Will it run on an old CPU? | `isa_scan.py exe out.json` (static), then Intel SDE `-<chip> -chip_check <XED chip>` on code that actually exercises the path, with a positive control (the old code must fail). `-snb`/`-wsm` alone only fake CPUID. |
| How big are the stack frames? | `frame_sizes.py exe out.json` (per-function `sub rsp` and `__chkstk` sizes). |

## 1. Old CPUs (Xeon) cannot start or crash in 3D

- Do not build the VU recompiler front end (kzvu/microVU) with `/arch:AVX2`: MSVC then emits VEX code anywhere,
  including static initialisers that run before `main` (pre-AVX Xeons died at start-up). PCSX2's baseline is SSE4.1;
  its JIT selects SSE4/AVX/AVX2 at run time.
- Also search for explicit `_mm256_*` / `__m256` intrinsics: compiler flags do not remove them. kzvu's VU1 code memo
  (`kzvuVu1CodeChanged`) ran a 256-bit test at the first VU1 program after any microcode upload, i.e. at the first 3D
  scene; it is now SSE4.1. A boot-only SDE run never reaches it: run kzvu's own `kzvu_test` / `kzvu0_test`
  (`-DKZVU_BUILD_TEST=ON`) under `sde -wsm -chip_check WESTMERE`.
- `ps2host::cpuMeetsBaseline()` logs `[cpu] brand | vendor | cores | ISA` and the runner refuses with a message box
  when SSE4.1 is missing, instead of an illegal-instruction crash.
- SDE artifact: under `-wsm` every DXGI `Present` fails ("OSXAVE BIT IS NOT SET"), even in a 60-line D3D11 program.
  Test that chip with the CPU raster (`PS2_GS_TEST_OPEN_FAIL=1`).

## 2. GPU, GS host service and presentation

- **GS open failure must not be a black screen.** Open the GS on its worker before the guest starts; on failure stop
  the worker, destroy the child window and let the CPU raster present (`PS2_GS_TEST_OPEN_FAIL=1` tests it). Log
  failed `Present` once with `GetDeviceRemovedReason`.
- **Readback:** PCSX2's `GSTextureCache::PrepareDownloadTexture` checked the address of the `unique_ptr` instead of
  the created texture (still in upstream); create into a temporary and keep the old texture on failure.
- **Which GPU renders:** the GS picks the high-performance adapter through `IDXGIFactory6::EnumAdapterByGpuPreference`
  (Windows 10 1803+) unless the port names one (`adapter=`). The EXE also exports `NvOptimusEnablement` and
  `AmdPowerXpressRequestHighPerformance` for older systems and OpenGL fallback presentation.
- **Internal resolution by GPU class** (`ps2host::gpuReport()` / `autoScaleCap()`): `gs_scale = auto` keeps the
  display-based choice (the smallest multiple of 448 lines covering the picture) and caps it at 4x on a discrete
  GPU, 2x on an integrated GPU (D3D12 reports unified memory; without D3D12, under 1 GiB dedicated memory; an APU can
  report a 2 GB carve-out as "dedicated"), 1x on the software renderer. It logs a `[gpu]` line. An explicit value
  (the player's choice) is never capped. Ship `gs_scale = auto` in a new package's settings so first runs adapt.
  `PS2_GPU_TIER=integrated|discrete|software` simulates a class in tests.
- **GS thread CPU:** in the measured battle the GS worker waited 89 % of the time (busy ~11 % of one core). Its hot
  spots were upstream PCSX2 code that is already vectorized or needed for correctness (`ClearGSLocalMemory`
  1.1 %, texture hashing, vertex min/max, hash-cache ageing); the port's bridge was 0.4 %. Profile before touching
  the GS; rebuilding it risks losing GS-side fixes, so diff the new DLL's strings against the deployed one.
- **Recommended, not yet done:** ship a pre-warmed `shadercache_pcsx2` (DX11 caches GPU-independent bytecode; first
  play then avoids most shader compile hitches; regenerate it whenever the GS DLL or GS options change), and expose
  the DX11 timestamp queries (`GSDevice11`) as GPU time in the performance line before any GPU work.

## 3. Movies (CG)

- **IPU (hardware MPEG decoder) games:** a starved IDEC used to restore its command-start snapshot and decode the whole
  frame again for every new piece of input (3-4x the work). Checkpoint the decoder at each macroblock boundary and
  resume there. Skip all-zero coefficient rows in the reference IDCT (adding +-0.0 never changes the sums). Both are
  bit-exact; in the measured opening movie IPU time fell from 1041-1452 ms to 423-536 ms per 5 s and the EE thread from
  40.6 % mean / 68.3 % p95 to 23.5 % / 28.2 %. With a 0.35-CPU budget the movie went from 41 to 58 fields/s.
- **MPEG HLE (FFmpeg) games:** feed `av_parser_parse2` from a copy with `AV_INPUT_BUFFER_PADDING_SIZE` zero bytes
  (SIMD start-code scans read past the end); use slice threads for MPEG-1/2 (`min(4, cores/2)`, `PS2_MPEG_THREADS=1`
  for one). Measure the `mpeg_decode` counter before moving decoding out of the global stub lock.

## 4. Host OS

- **No EcoQoS:** `SetProcessInformation(ProcessPowerThrottling)` with execution-speed throttling off (and timer
  resolution kept, where Windows accepts the flag). Otherwise Windows may run an unfocused or battery-powered port
  on efficiency cores at low clocks.
- **Thread priority:** EE and GS threads one step above normal on 4+ logical CPUs (`PS2_THREAD_PRIORITY=0` = normal).
  Measured with 12 busy background processes: 30 game frames/s with it, 13-18 without. It shifts host timing enough
  to change ~100 bytes of loading-time guest state compared with normal priority (the fight itself did not diverge);
  netplay peers must use the same build and settings.
- **Stack:** generated functions can have very large frames (0.7 MB in the measured port; nested calls overflowed a
  1 MB stack). Link the runner with `/STACK:16777216` (address space only).
- **Crash reports:** an unhandled-exception filter and `std::terminate` handler write `logs\crash-<time>.dmp` and a
  `[crash]` line (code, module+offset); keep the newest few; `PS2_TEST_CRASH=<s>` tests it. In the measured work it
  caught a real stack overflow on its first day.

## 5. Runtime hot paths (EE thread)

Typical battle profile of the EE thread: ~50 % waiting, guest AOT code ~19 %, runtime ~11 %, dispatch/scheduler ~7 %,
microVU output ~6 %, memory/DMA ~4 %, GIF/VIF ~3 %.

- Remove per-call costs that are not work: diagnostic hooks evaluated on every guest call (read their switches once
  at start-up), function-local `static const x = getenv(...)` in per-block paths (MSVC checks a TLS init guard on every
  call; use plain statics read on first use, after the PS2_ environment aliases are installed).
- `dispatchGuestBranch`: on a direct hit in the generated function table, one probe instead of
  `hasFunction()` + `lookupFunction()`. Keep the scheduler checkpoint at every call (guest timing) and the table
  indirection (game overrides, the patched-ELF interpreter and relocatable modules depend on it).
- Link-time code generation for the runtime library only (`INTERPROCEDURAL_OPTIMIZATION_RELEASE` on `ps2_runtime`,
  `/LTCG` on the runner); not for 10k generated files. `/GS-` for generated code.
- **Do not `__forceinline` the per-instruction helpers** into generated code. On a 32k-line generated function:

  | Header variant | Stack frame | Instructions |
  | --- | --- | --- |
  | original | 689 KB | 477k |
  | all helpers forced inline | 1,607 KB | 937k |
  | only the 3 register helpers | 1,126 KB | 715k |
  | only the address checks | 740 KB | 551k |
  | none forced; wrapped RAM access moved out of line | 689 KB | 465k |

  The forced build overflowed its stack in every battle. Test header changes by compiling the largest generated file
  alone and reading its frame size.
- GIF: skip whole PACKED blocks without an A+D descriptor when scanning for SIGNAL/FINISH/LABEL. VIF1 UNPACK V4-32
  without mask/mode: 16-byte copies, checked with the differential harness `tools/diag/vif1_unpack_diff`
  (3,000,000 random trials equal).
- Result in the measured battle: EE thread mean 50.5 % -> 46.9 % over both rounds; small, as expected for these
  items. Not done: skipping the guest's VSync spin (a game-profile item; host catch-up already bounds it), direct
  AOT-to-AOT calls (needs regenerating all code and the override/interpreter paths), lazy VU0 MAC flags.

## 6. Player-facing diagnostics

- A one-line performance overlay through the GS on-screen text, once a second:
  `30 fps (60 fields/s) | EE 48% | GS 10% | D3D11 3x`. EE near 100 % of a core = CPU-bound; frames halved with steady
  fields = the guest is late; GS high with EE low = GPU or driver.
- A diagnostics package (zip: summary with CPU/OS/memory/GPU/renderer/recent samples/crash count, the run log, GPU
  log, settings, newest crash dumps; never saves). Both are services of `ps2_host_report` (`setOverlay()`,
  `exportDiagnostics()`); a port wires them to its own settings or menu. `PS2_PERF_OVERLAY=1` turns the overlay on in
  hidden tests.

## 7. Checklist for a new port

1. Static ISA scan of the EXE and DLLs; SDE chip-check of start-up and of the VU tests.
2. GS-open-failure fallback (`PS2_GS_TEST_OPEN_FAIL=1`); `[cpu]`, `[host]`, `[gpu]` lines present.
3. Per-thread CPU in a movie and in the heaviest gameplay; profile the limiting thread.
4. Bit-exact hashes for any decoder change; cross-build determinism (with its same-build noise floor) for runtime changes.
5. Weak-host comparison (old vs new side by side) and a background-load run, reading game frames/s.
6. Frame-size scan after any change to headers that generated code includes.
7. Crash-dump test; diagnostics package test.
8. Keep claims to what ran: one PC is not "all PCs"; a boot-only SDE run is not a 3D run.

## Switches (tests and support)

`PS2_GS_TEST_OPEN_FAIL=1`, `PS2_TEST_CRASH=<s>`, `PS2_THREAD_PRIORITY=0`, `PS2_GPU_TIER=...`, `PS2_PERF_OVERLAY=1`,
`PS2_MPEG_THREADS=<n>`, `PS2_IPU_IDEC_RESTART=1` and `PS2_IPU_IDCT_REF=1` (old IPU paths for A/B), `DOD_PERF=1`
(stage counters, IPU report and hashes), `DOD_FPS=1` (frames/fields per 300 fields).

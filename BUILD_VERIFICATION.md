# Build verification (2026-10-27, repeated 2026-10-28)

**2026-10-28 update (rows 146–150):** the commit was cloned into an empty folder, `fetch_deps.ps1` extracted the same
pinned zips (all SHA256 matched; PCSX2 `646df006a4` reused), and `build_all.ps1` from an empty build folder built
`kzvu.lib`, `pcsx2gs.dll` and all of PS2Recomp. Results are unchanged from 2026-10-27: IOP tests all pass, GS cache
45 / 45, `ps2x_tests` 421 / 446 with the same 25 known differences (section 3, same test names). The new code
(particle edges, GPU choice, background HD movie decoding, capture, menu API v3, cheat menu) was tested in the original
game's port, not by these tests. The row 151 commit (HD movie route hang) was verified the same way, with the same
results; its behaviour was tested in the port (an injected sequence end at pictures 28 and 1000, a shorter HD file,
movies back to back: every movie plays and the game goes on).

Environment: Windows 10 Pro 22H2 (19045) x64, Visual Studio 2022 Build Tools (MSVC 19.44.35229), CMake 3.31.6,
Windows PowerShell 5.1.

## 1. Fresh clone + `fetch_deps.ps1`

The files git tracks under `.gitignore` (796 files, 37 MB) were copied to an empty folder and `fetch_deps.ps1` was run
there:

- all 8 dependency zips matched their pinned SHA256 (raylib, ELFIO, toml11, fmt, libdwarf, rabbitizer, nlohmann/json,
  SDL2 VC package); PCSX2 was fetched at commit `646df006a4688063e56c0826e7ea4b8bb0c112c4`
- the extracted folders are file-for-file identical to the ones the build below used (PCSX2 compared without `.git`)
- `build_all.ps1` in that fresh folder (FFmpeg downloaded by CMake, same SHA256 as the local copy
  `b985887dbca8c0a9…`) built everything and gave exactly the test results of section 2

Upstream's own `.gitignore` (in `third_party/src/PS2Recomp`) ignores `ps2xRuntime/src/runner/`, so the two files this
base keeps there (`register_functions.cpp`, `dod_utf8.manifest`) are committed with `git add -f`; do the same for your
game's `main.cpp`.

`patches/PS2Recomp_vs_upstream_eb8688b.diff` applied with `git apply -p1` to upstream `eb8688b` reproduces
`third_party/src/PS2Recomp` exactly (apart from line endings).

## 2. `build_all.ps1` from an empty build folder

| Step | Result |
|---|---|
| `kzvu.lib` | built |
| `pcsx2gs.dll` | built |
| PS2Recomp `ALL_BUILD` | built: `ps2_recomp.exe`, `ps2_analyzer.exe`, `ps2_runtime.lib`, `ps2EntryRunner.exe`, all test programs |
| IOP tests (`ps2_iop_compatibility_tests` 18/18, `ps2_iop_emulator_tests`, `ps2_iop_import_tests`, `ps2_iop_import_version_tests` 6/6, `ps2_iop_spu2_tests`) | all pass |
| GS cache tests (texture, CLUT, memory: one process per registered test name) | 45 / 45 pass |
| `ps2x_tests` | 421 / 446 pass, 25 known differences (below) |

Fixed while preparing this base: `ps2_gs_clut_cache_tests` did not link, because the runtime's `gs_frontend.cpp`
calls the PCSX2 GS bridge, which this CPU-backend test suite does not build. `ps2xTest/gs_cache/gs_cache_no_pcsx2_gs.cpp`
stands in for the absent bridge.

## 3. The 25 known `ps2x_tests` differences

Upstream `eb8688b` itself fails 1 of these tests (the first row). The rest expect upstream's behaviour where this
base deliberately follows the hardware, PCSX2 or the original SDK. Each was reviewed against its source change
(`docs/DoD_UPSTREAM_TEST_REVIEW_20261020.md`, `docs/DoD_RUNTIME_PATCHES.md` rows in parentheses); none is a regression.

| Test | Why it differs |
|---|---|
| VU0 macro mappings cover all S1/S2 enums | Test environment: reads `instructions.h` relative to the working directory (fails upstream too) |
| VU random helpers emit line comments on separate lines | VRNEXT/VRINIT/VRXOR were rewritten as PCSX2's LFSR; the test looks for comment text of upstream's old implementation |
| unresolved JALR marks internal labels as indirect fallback resume entries | Performance: only return addresses of indirect calls become entries (41) |
| GIF PACKED A+D writes DISPFB1 and DISPLAY1 privileged registers | Hardware: A+D addresses 0x59–0x5F are not GS registers (33) |
| GIF PACKED A+D writes DISPFB2 and DISPLAY2 privileged registers | Same |
| GS CSR/IMR support coherent 64-bit and 32-bit access | Hardware: CSR event bits are write-1-to-clear, FIELD/FIFO read-only |
| sceGsSyncV resumes through the scheduler with deterministic field parity | Reads the original game's libgraph record at a fixed address (IMPROVEMENTS.md §12) |
| sceGsSyncVCallback runs as a scheduler invocation on its callback stack | Handler stacks moved below the ELF so they no longer overwrite game stacks (14, 137) |
| GIF DMA chain IRQ stops only when TIE is set | PCSX2: GIF state persists across transfers; the test reuses one instance |
| GIF arbiter prioritizes PATH1 then PATH2 then PATH3 | Deliberate (upstream #299): submission order, so texture uploads precede their draws; `DOD_GIF_PRIORITY_SORT=1` restores sorting (73) |
| VIF FBRST RST clears VIF1 command state | PCSX2: RST does not clear CYCLE/MODE/NUM/MARK (102) |
| VIF STMOD offset and difference modes apply to UNPACK data | PCSX2: the test never sets STCYCL, and CYCLE=0 unpacks nothing (43) |
| VIF UNPACK V4-16 sign and zero extension follow immediate bit14 | Same |
| VIF UNPACK bit15 adds TOPS to destination address | Same |
| VIF UNPACK num zero uploads 256 vectors | Same |
| VIF masked UNPACK uses data row col and protect selectors | Same |
| VIF1 DIRECT finds an image continuation after packed setup | Hardware: data outside a DIRECT's declared size is the next VIFcode (104) |
| VIF1 DIRECT image tag can continue with raw image qwords | Same |
| VIF1 packet builders keep chain qwc live before terminate | Original SDK: libvifpk adds the QWC once when the packet is closed (113) |
| IPU init executes its guest helper as a scheduler invocation | PCSX2 IPU: IPU_CTRL.RST triggers a reset and does not read back (29, 54) |
| IPU init skips missing optional helper instead of dispatching the default trap | Same |
| thread lifecycle, nested suspend, WAIT-SUSPEND, and wakeup count are centralized | Hardware: SuspendThread returns the thread id (64) |
| SifInitRpc does not reset the running IOP | Performance: the IOP runs in 4096-cycle quanta on its own thread (77, 78); the test advances 80 cycles |
| SifCallRpc falls back to stack ABI when register pack is implausible (error) | A blocking sceSifCallRpc sleeps the caller through the scheduler as on hardware (16); the test has no scheduler loop |
| register bind call updates descriptors and payload (error) | Same |

## Not covered

- Only this machine; no other PC or clean Windows install.
- No game was recompiled from this base folder: the original game's port was built and tested from the same sources
  in its own project, and another game needs its own testing.

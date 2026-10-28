# Improvements over upstream PS2Recomp (ran-j/PS2Recomp `eb8688b`)

[繁體中文](IMPROVEMENTS.zh-TW.md)

Full diff: `patches/PS2Recomp_vs_upstream_eb8688b.diff` (97 files, +17,262 / −494 lines; `git apply -p1` on upstream
`eb8688b` reproduces this tree apart from line endings). The cause and verification of every item are in
`docs/DoD_RUNTIME_PATCHES.md` (Chinese); the numbers in parentheses are its row numbers. Most changes in the code carry
a `[DoD]` comment. **That is only a history marker, not "specific to that game"**: the genuinely game-specific parts
are listed in the last section.

## 1. Recompiler (ps2xRecomp): correctness of the generated C++

- MMI / FPU / VU0 macro semantics checked against PCSX2: PMFHL.LW, PEXT/PPAC families, FPU compares and MIN/MAX,
  VU0 FMAC updates the MAC flags, VCLIP sign bits, CFC2/CTC2 R-register masks, VRNEXT/VRINIT/VRXOR as PCSX2's LFSR
  (39, 41, 70, 143)
- BLEZ/BGTZ/BLTZ/BGEZ (incl. likely/AL forms) compare all 64 bits (56)
- Functions reached only through pointers get entry points: `tools/find_missing_function_starts.py`,
  `scan_code_pointers.py`; switch jump-table targets: `scan_jump_tables.py` (42, 83)
- Fragment functions dispatch through their parent's entry: `prefer_parent_entries.py` (123)

## 2. EE kernel, threads, interrupts

- Thread priority 0 allowed, ChangeThreadPriority returns the old priority, Suspend/WakeupThread return the thread
  id (13, 15, 64)
- VSync/alarms advance in real time instead of stalling at ~2 Hz when kernel calls count no cycles (17); NTSC
  59.94 Hz VBlank (76); host time no longer "repaid" faster than real time after slow phases (88, 99)
- DI/EI critical sections are not interrupted or preempted (66); masked interrupts stay pending until re-enabled
  (52); INTC_STAT write-1-to-clear (45)
- A checkpoint yield at a recursive entry is no longer mistaken for a return (44)
- Interrupt/alarm handler stacks no longer carved from the top of RAM (over game stacks) and are reclaimed (14, 137)
- VU0 registers per guest thread, but the random register R shared by all threads as on the single real VU0; vf0
  initialised to (0,0,0,1) (58, 143)
- SetupHeap limit; SIF command packets no longer allocated from the game's heap (72, 74)
- Hang dumps include the guest thread table (139)

## 3. DMA, memory, timers

- IPU DMA ch3/ch4, SPR DMA chain mode, DMA address bit 31 (25, 26, 51)
- D_ENABLEW → D_ENABLER, CHCR reads keep STR, D_STAT clear semantics (53, 65, 52)
- Third-level nested DMA chain CALL (114); chains over 4096 tags not truncated (43)
- DMAC completion interrupts dispatched after writes of any width (71)
- EE timer T3 counts H-BLANKs; lazily evaluated timers (46, 118)
- GS CSR.FIFO read value (69)

## 4. VIF / GIF / GS

- VIF1 UNPACK semantics as PCSX2 (WL=0, fill mode, V2/V3/V4-5, STMOD, masks), with pre-selected fast paths (43, 121)
- VIF1 chains read data as they execute, so data written by a SIGNAL handler is seen (28)
- GIF FLG=3, IMAGE continuation across transfers, PATH3 continuation state cleared on reset (34, 108)
- GS registers 0x59–0x5F are not privileged registers (33); GS SIGNAL/FINISH raise EE interrupts (27)
- Root cause of libvifpk replacements rewriting the open DMA tag's QWC (113)
- sceGsResetPath / sceVpu0Reset really reset (102); GsSetCrt writes SMODE1 (38)

## 5. PCSX2 hardware GS (pcsx2gs.dll) and VU1 (kzvu)

- `third_party/src/pcsx2gs`: PCSX2 `2954ebe`'s GS (DX11/DX12) as a DLL with the C API `pcsx2gs_api.h`; the runtime
  side `gs/dod_pcsx2_gs.cpp` feeds it on a separate GS thread and presents in a child window (36, 37, 67)
- GS fixes: PACKED continuation overrun, Target::Update crash, field offsets (60, 90, 100); HD texture replacement
  with an upload budget, retention, multi-threaded loading and scene prefetch (129, 140); FXAA/SSAA (130); crash at
  window close (131)
- Fullscreen/borderless: only the D3D child window presents (the parent no longer swaps black OpenGL frames);
  exclusive-fullscreen focus checks use the top-level window (145)
- Upscaling: no more 1-pixel lines around effect sprites (fractional coordinates, bilinear, texture atlases; the
  same family as PCSX2 #5851 / #6396). For scene particles the sprite's texture rectangle is pulled in by half a texel
  of the coarsest mip level it actually reads, with or without mipmaps (`DoDNativeParticleEdges`;
  `DOD_PARTICLE_EDGES=0` turns it off) (146)
- GPU choice: `gs_options` `adapter=<name>`; otherwise the Windows graphics preference for the executable, otherwise
  the high-performance hardware adapter (D3D11 used to take adapter 0, the integrated GPU on many laptops); the GPU in
  use is written to `logs\gpu.txt` (147)
- Menu API v3 for the host (`pcsx2gs_api.h`): a panel docked at the left (`begin_panel`), tooltips, the system's
  CJK font merged into the ImGui font (150)
- `third_party/src/kzvu`: PCSX2 microVU (646df006a4) as a static library replacing upstream's VU1Interpreter (50);
  the fallback interpreter no longer cuts programs off at 65536 cycles (138)

## 6. IPU / MPEG movies

- Full IPU (AC5-Recomp's `ps2_ipu.c`, Apache-2.0, via urban-reign-recomp): IDEC/BDEC/VDEC/CSC; snapshot replay no
  longer drops data (24, 40, 54, 55)
- sceIpuInit HLE no longer hard-codes another game's addresses (29); libdma Recv family (31)
- FFmpeg-backed HLE MPEG path (`PS2X_ENABLE_FFMPEG`)
- HD movie replacement (`dod_hd_cg.h`) decodes up to 8 frames ahead on a background thread: the game thread no longer
  decodes 1080p HEVC while it holds the MPEG lock, so playback and sound no longer hitch (149)

## 7. IOP (ps2xIOP, runs the original IRX modules)

- loadcore `start(argc, argv)`, SIFMAN initialised at boot, sceSifBindRpc / GetOtherData, EE→IOP RPC through real
  server threads (3, 4, 5, 20, 47)
- sysclib look_ctype_table, wmemcopy/wmemset sizes (19, 61); stdio printf formatting (7)
- No DMA completion or thread switch inside CpuSuspendIntr critical sections (68); LWL/LWR in delay slots, heap
  reuse, WaitSema/DelayThread outside threads (75)
- IOP on its own host thread (78, 84)
- **SIO2 + DualShock 2** with rumble (32, 127)
- **SPU2**: voices, ADPCM, Gaussian interpolation, envelopes and volume slides as PCSX2, AutoDMA rate, reverb
  (85, 89, 106, 111, 112, 115)

## 8. CD / DVD

- EE `sceCdSearchFile` indexes the image's ISO9660 directory with real LBNs; sceCdCallback / sceCdRead notify (8, 9)
- Dual-layer DVD9 layer 1 (95); disc swapping via sceCdTrayReq (80, 81)

## 9. Host

- Gamepads: SDL2 loaded dynamically, Sony pads, rumble (`dod_gamepad.cpp`) (127)
- Frame pacing: one deadline pacer, VSync off, game clocks untouched (128, `dod_host_pacer`)
- UTF-8 process code page manifest: runs from folders with any characters (144)
- Master volume (`dod_audio_settings.h`), key binding parser (`dod_input_settings.h`)
- Hybrid-GPU laptops: the example runner exports `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`
  (copy them into your own runner's `main.cpp`) (147)

## 10. Performance

IOP advancing, chain buffer pool, cached getenv (77); timers (118); GS command chunks not zero-filled (120); VIF1
UNPACK fast paths (121); scheduler fast hops (122).

## 11. Diagnostics (environment variables; inactive unless set)

`DOD_PERF`, `DOD_FPS`, `DOD_FRAME_CAPTURE_DIR`, `DOD_GS_CAPTURE`, `DOD_LAYER_LOG`, `DOD_VIF_EVENTS`,
`DOD_ADMA_WATCH`, `DOD_SPU2_WAV` / `DOD_SPU2_STATS`, `DOD_RAM_DUMP`, `DOD_HANG_DUMP_AT`, `DOD_RNG_TRACE`,
`DOD_PAD_SCRIPT`, `DOD_TEST_AUTOSTART`, `DOD_TEST_EXIT_AFTER`, `PS2X_PRESENT_STATS` and more.
`DOD_LINE_CAPTURE_DIR` (F8 or `DOD_LINE_CAPTURE_AT=<seconds,…>`): the GS output plus PCSX2 dumps of every draw of the
next frames and per-draw logs (`dod_draws.tsv`, `dod_sprites.tsv`, a summary that classes draws as 3d / particle /
world-sprite / hud / post / fill / cg; `DOD_LINE_CAPTURE_MODE=light|ab`) (148). `DOD_PRESENT_SHOT=<dir>:<first>:<every>:<count>`:
the final presented image, menus included (150). Scripts in `tools/`:
disassemblers, instruction/HLE/IOP-import census, GIF parsing, thread profiling, WAV analysis, GitHub issue fetching.

## 12. Specific to the original game: change or remove when porting another one

| Where | What | For another game |
|---|---|---|
| `examples/dod_runner/main.cpp` (not built) | The original game's entry: ini file, hooks by address, disc swap, widescreen switch, RNG trace, HD texture scene prefetch (scene number at 0x7E1344) | Use as a template for your own `ps2xRuntime/src/runner/main.cpp` |
| `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp` `sceGsSyncV` | Hard-coded libgraph record address `kDodGsGParamAddress = 0x003FD4A0` | **Must change**: the address your game's `sceGsGetGParam` returns, or stop HLE-ing this function |
| `ps2xRuntime/src/lib/Kernel/Stubs/dod_hd_cg.h` (used by MPEG.cpp) | 1080p movie replacement from `movies_hd\movies.tsv` next to the executable | Falls back before playback when the folder is absent; `DOD_CG_HD_DIR=0` disables it |
| `ps2xRuntime/src/lib/gs/dod_cg_cadence.h` | Movie frame cadence, fed by hooks in the original game's generated code | A new game has no such hooks; `DOD_CG_CADENCE=keep` disables it |
| `ps2xRuntime/src/lib/gs/dod_widescreen.h`, pcsx2gs `GS/DoDWidescreen.h` and the widescreen block in `GSState.cpp` | The original game's 16:9 (projection constants live in its generated code) | Only on when the runner passes `widescreen=1`; leave it off |
| pcsx2gs `GS/DoDHdStatic.h`, `DoDHdCg.h` | GS side of that game's HD static pages and movies | Depend on that game's textures/movies |
| `ps2xRuntime/CMakeLists.txt` | `dod_branding.rc.in` (icon resource, skipped when absent), `dod_utf8.manifest` (generic, keep) | Put your icon resource at `src/runner/dod_branding.rc.in` |
| `ps2xRuntime/src/lib/dod_cheats.[h,cpp]` (called from `ps2_runtime.cpp` and `ps2_pad.cpp`; mouse input in `gs/dod_pcsx2_gs.cpp`) | F2 in-game cheat menu for the original game: item counts written to its RAM addresses each frame; instruction cheats are `g_dodCheats[n]` switches that a script inserts into that game's generated code (not part of this repository). Everything starts off | The menu itself is generic (left panel, mouse / keyboard / controller, follows the image size): replace the entries and addresses with your game's, or remove the file and its two calls |

Many `[DoD]` fixes were found with that game but follow PCSX2 or hardware behaviour (SPU2, IOP critical sections,
VIF1…) and apply to other games as well. If a new game behaves differently, check the matching row of
`docs/DoD_RUNTIME_PATCHES.md` for its explanation and environment switch.

# PS2 static-recompilation and native-port ecosystem survey (2026-11-12)

[繁體中文（完整版）](ECOSYSTEM_SURVEY.zh-TW.md) - the Traditional Chinese document is the complete version, with the HEAD
commit of every repository surveyed. Compatibility and progress figures quoted from READMEs are the projects' own claims
and were not verified here.

## Method

28 repositories (plus the BozoEngine submodule of item 19) were cloned with `--depth 1 --filter=blob:none`; only tree
listings and selected files were read, nothing downloaded was built or run. Content identity uses git blob ids
(identical id = identical file). Baselines: PS2Recomp `eb8688b` (this base's upstream start, pinned zip), PS2Recomp
HEAD `e5636981a1`, this repository's `main` (`5e907ef`) and PCSX2 HEAD. Each file is credited to its earliest source.

## Lineage

| Repository | Files | Same as PS2Recomp `eb8688b` | Same as this base | Lineage |
|---|---:|---:|---:|---|
| ran-j/PS2Recomp HEAD | 298 | 292 | 223 | 6 files changed since `eb8688b` (GIF FLG=3, #384; ported here) |
| gta-vcs-recomp | 594 | 223 | 189 | vendors PS2Recomp |
| Tobiichi-Port | 581 | 214 | 187 | vendors PS2Recomp |
| urban-reign-recomp | 862 | 140 | 465 | origin of this base's runtime and PCSX2 GS bridge lineage |
| BT3-Recomp | 665 | 76 | 69 | PS2Recomp fork + recompiled IRX |
| KillzoneRecomp | 346 | 0 | 9 | origin of kzvu (PCSX2 microVU); archived |
| BT2-Recomp | 572 | 0 | 0 | uses PS2Recomp `c5a9d02` as a tool, own native layer |
| gt4-staticrecomp | 1496 | 0 | 1 | independent recompiler and runtime |
| ps2-web-recomp / ps2-native-recomp | 116 / 232 | 0 | 0 | independent lineage; native is a fork of web |

## ps2-native-recomp vs ps2-web-recomp

native's README states it is built on ps2-web-recomp (MIT). Of web's 116 files, native keeps 104 paths (17 identical,
87 modified) and drops 12 (GT3 tests and media); it adds 128 files. Of web's 27,671 code lines native keeps 23,790,
adds or changes 17,515 in shared files and removes 3,195, and adds 34,842 lines in new files (runtime 10,724, tests
8,848, scripts 6,651, host 3,484, app 3,308, recomp 1,761). Count web's base once; native's own contribution is about
52k lines: the native Windows/Linux host, GPU GS changes, SIF/SIO2/IOP CPU fixes, VU operations, recorded-then-compiled
VU0/VU1 microprograms, multi-game autotests and a self-bootstrapping app. Shared architecture from web: `eerecomp`
control-flow function discovery in 16 KiB units, HLE EE kernel, pipeline-accurate VU1, WebGPU compute GS on swizzled
VRAM, SPU2 rewrite, interpreted IRX.

## IOP: three different routes

| Route | Example | Disc IRX | Main risk |
|---|---|---|---|
| HLE service replacement | BT2-Recomp `runtime/services/` (SIF RPC, sound, module, CRI audio, IOP heap) | not executed; EE RPC answered natively | every game's RPC protocol must be rewritten |
| Interpreted IRX | this base `ps2xIOP`, ps2-native/web-recomp, Play! (HLE BIOS) | loaded and interpreted on an R3000 core | interpreter cost; kernel services must follow ps2sdk |
| Recompiled IRX | BT3-Recomp `iop_native/<module>/` (19 modules) | IRX ELF parsed, functions registered at module-relative addresses | MIPS-I compiled with the EE (R5900) translator: R3000 load delays not modelled; generated code derived from disc modules |

BT2 also has an isolated diagnostic R3000 translation prototype that stops at SIO2MAN's SIO2 reset write and records
that no module success is fabricated. This base keeps interpretation as the default, adds HLE only for ABIs with public
references and tests (the new `thmsgbx`, from ps2sdk), and treats recompiled IRX as a possible opt-in accelerator.

## VU and GS

VU: this base uses PCSX2 microVU through kzvu; gt4-staticrecomp precompiles every VU0/VU1 microprogram; ps2-native-recomp
records microprograms and compiles them ahead of time with an interpreter fallback. GS: PCSX2 hardware GS DLL (this
base, urban-reign lineage); CoreGS 2.0 Vulkan with out-of-process GSdx fallback (GT4); paraLLEl-GS (Def Jam, BT3
experimental); OpenGL (BT3); D3D11/Vulkan (GTA VCS, Burnout 3); WebGPU compute (ps2-web/native).

## Item 19: ChampionsOfNorrath_Recomp

Exists and is accessible (HEAD `fb55499314`, 2026-11-11), status badge "bring-up". 32 files of game-specific code and
tools; the engine is the submodule TheBozos/BozoEngine (`a36a4d2`, 291 files, own recompiler and `tools/native_irx`).
Recorded as a project to watch, not as an established technical asset.

## Taken now and next candidates

Taken: see [IMPROVEMENTS.md](../IMPROVEMENTS.md) section 13. Candidates, each needing its own design, tests and
acceptance: a generic overlay registry (now implemented, [IMPROVEMENTS.md](../IMPROVEMENTS.md) section 14); recorded
ahead-of-time VU microprograms with the kzvu JIT as fallback; an opt-in recompiled-IRX accelerator with MIPS-I load-delay
semantics validated against the interpreter; multi-game automated regression; a Vulkan GS backend for Linux.
Decompilation/rewrite projects (rac1-native, OpenGOAL, GT3-revisited, ProjectCane, rac-rs, recvx, kh1, mh1j,
Tenkaichi3Decomp) share no code with this base; their symbols and function boundaries can feed a port's function map.

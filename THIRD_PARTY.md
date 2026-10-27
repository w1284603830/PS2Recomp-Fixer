# Third-party code and licenses

This repository as a whole is distributed under the **GNU General Public License v3.0** (`LICENSE`), because it
contains GPL-3.0 code (PS2Recomp, PCSX2, KillzoneRecomp, urban-reign-recomp). Every component keeps its own license
file next to its source.

## In this repository

| Path | What | Origin | License |
|---|---|---|---|
| `third_party/src/PS2Recomp/` | Static recompiler, runtime, IOP emulation, tests — **modified** (`patches/`, `IMPROVEMENTS.md`) | [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) commit `eb8688b2fd191129f103a39543002c005b33409c` | GPL-3.0 (`third_party/src/PS2Recomp/LICENSE`) |
| `third_party/src/PS2Recomp/ps2xRuntime/src/lib/ipu/` | IPU (MPEG decoder) emulation | AC5-Recomp `runtime/src/ps2_ipu.c`, `ps2_ipu_dct.inc`, via urban-reign-recomp | Apache-2.0 (`docs/licenses/AC5-Recomp-APACHE-2.0.txt`), GPL-3.0 compatible |
| `third_party/src/PS2Recomp/ps2xRuntime/src/lib/gs/dod_pcsx2_gs.cpp` | Runtime side of the PCSX2 GS bridge | Adapted from [thiagordsfx/urban-reign-recomp](https://github.com/thiagordsfx/urban-reign-recomp) `gs_pcsx2_backend.cpp` | GPL-3.0 |
| `third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_ipu_vlc.h` | MPEG-2 VLC tables | PCSX2 `IPU_mpeg2_vlc.h` | GPL-3.0-or-later |
| `third_party/src/pcsx2gs/pcsx2/` | PCSX2's hardware GS (DX11/DX12), `common/` and their libraries (fmt, imgui, cpuinfo, D3D12MA, xbyak, fast_float, WIL, xxhash) — only the files the build uses, some **modified** (marked `[DoD]`) | [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) commit `2954ebe` | GPL-3.0-or-later (`pcsx2/COPYING.GPLv3`); each library's license is in its folder |
| `third_party/src/pcsx2gs/pcsx2-gs/` | Builds the above into `pcsx2gs.dll` with a C interface, host stubs, shaders and font | urban-reign-recomp's `thirdparty/pcsx2` layout, extended here | GPL-3.0 |
| `third_party/src/pcsx2gs/directx-headers/` | DirectX-Headers `include/` | [microsoft/DirectX-Headers](https://github.com/microsoft/DirectX-Headers) commit `461ef51` | MIT |
| `third_party/src/kzvu/` | PCSX2 microVU as a static library (unmodified) | [drakolordx7/KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp) `ext/kzvu` | GPL-3.0 (`third_party/src/kzvu/LICENSE`) |

Several runtime fixes follow PCSX2's behaviour or were cross-checked against the issues, PRs and notes of other
PS2Recomp-based projects (KillzoneRecomp, urban-reign-recomp, god-of-war-recomp, sotc-vibe-pc, godhand-recomp and
others); the source comments name the project and patch where code or a technique came from.

## Downloaded by `fetch_deps.ps1` (not in the repository)

| Folder | Version | Source | License |
|---|---|---|---|
| `third_party/src/raylib` | 5.5 | [raysan5/raylib](https://github.com/raysan5/raylib) | Zlib |
| `third_party/src/SDL2-2.30.9` | 2.30.9 VC development package | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | Zlib |
| `third_party/src/elfio` | Release_3.12 | [serge1/ELFIO](https://github.com/serge1/ELFIO) | MIT |
| `third_party/src/fmt` | 12.1.0 | [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT |
| `third_party/src/rabbitizer` | 1.14.3 | [Decompollaborate/rabbitizer](https://github.com/Decompollaborate/rabbitizer) | MIT |
| `third_party/src/toml11` | v4.4.0 | [ToruNiina/toml11](https://github.com/ToruNiina/toml11) | MIT |
| `third_party/src/libdwarf` | v2.2.0 | [davea42/libdwarf-code](https://github.com/davea42/libdwarf-code) | LGPL-2.1 (the library; see its `COPYING`) |
| `third_party/src/nlohmann_json` | v3.11.3 | [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| `third_party/_downloads/pcsx2_646df006a4` | commit `646df006a4688063e56c0826e7ea4b8bb0c112c4` | [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) (kzvu compiles its VU code from it) | GPL-3.0-or-later |
| FFmpeg (downloaded by CMake) | n7.1-241205 LGPL shared | [System233/ffmpeg-msvc-prebuilt](https://github.com/System233/ffmpeg-msvc-prebuilt) | LGPL-2.1-or-later |

Pinned SHA256 values are in `fetch_deps.ps1` and `third_party/sources.lock.json`.

## No game content

Nothing in this repository comes from a PlayStation 2 game disc: no executables, data, textures, audio or video.
The names `DoD` / `dod_` in the code and the notes in `docs/` come from the project this base was developed in (a PC
port of *Shin Onimusha: Dawn of Dreams*); that port and its game-derived code are **not** part of this repository.
PlayStation 2 is a trademark of Sony Interactive Entertainment; this project is not affiliated with Sony, Capcom or
the PCSX2 team.

# Host timing, latency and texture maintenance

These shared services do not identify a game, select guest addresses, or change EE/IOP/SPU clock rates.
Configure options before starting the runtime. Read [REGRESSION_TESTING.md](REGRESSION_TESTING.md) for release acceptance.

| Control | Default | Purpose |
|---|---|---|
| `PS2_GS_WAIT_PACING=legacy` | Measured-wait compensation | A/B against the old host-deadline policy. The asynchronous GS producer's blocked time shifts outstanding host deadlines; guest deadline cycles and event order stay intact. |
| `PS2_GS_DRAIN_US=0..2000` | `0` | Optional extra host pacing per field while at least two fields are queued. Select and qualify this per port; no game's drain interval is universal. Ignored in legacy mode. |
| `PS2_DXGI_MAX_FRAME_LATENCY=0..16` | No override | DX11 device queue control. `1..16` sets a limit, `0` asks DXGI to restore its default, and `default` leaves it untouched. Unsupported interfaces/API failures are reported; normal device creation continues. |
| `PS2_TEST_PRESENT_PERIOD_US=1..1000000` | Disabled | Diagnostic presentation limiter for reproducing display/driver backpressure. Never enable in a release preset. |
| `DOD_PERF=1` | Disabled | `[perf:gs-wait]` blocked time/queue depth and `[gs-slow]` commands taking at least 10 ms. |
| `DOD_TEXTURE_TRACE=1` | Disabled | Synchronous texture decode/upload times and existing replacement-selection evidence. |
| `PS2_SHADER_TIMING=1` | Disabled | DX11 shader compilation time, stage, entry point and success; cache keys and shader variants are unchanged. |

Unsigned numeric options reject signs, trailing characters and overflow. With host limiting disabled (`fps_limit=0`),
an asynchronous GS uses a blocking 1 ms window/input-pump wait. The synchronous GS and CPU-renderer paths retain their
existing pacing selection. This is host polling, not a new guest frame limit.

The DXGI range follows Microsoft's [IDXGIDevice1::SetMaximumFrameLatency contract](https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-setmaximumframelatency).
A queue limit is not a measured button-to-screen latency or a guarantee for a GPU family.

## UI decode manifest

This predecode path was originally developed as an optimization for **HD texture replacement porting**. The base
retains the reusable loading mechanism; each port supplies its own texture pack, atlas list and memory budget.

A pack may supply `<texture directory>/ui_preload.txt`, one replacement basename **without an extension** per line.
Blank lines, surrounding whitespace, UTF-8 BOM and `#` comment lines are accepted. Duplicate names are ignored;
paths and malformed tokens are rejected. The file is limited to 1 MiB and 1,024 distinct lexical entries, with at
most 128 bytes per name. Only validated names already discovered directly in `replacements/pc_ui_hd` are admitted.
The repository supplies no game-specific manifest or texture names.

UI decoding runs on the existing loader threads, independently of the room prefetch list and full-pack precaching.
Only the CPU copy is preloaded; normal first-use lookup uploads the selected texture. If decoding is unfinished,
fails or the CPU copy was evicted, lookup retains its synchronous fallback. Completion racing that fallback counts
cache bytes once. Existing `texture_threads` and `texture_cache_mb` settings apply; preloading does not pin textures
or create an unlimited separate cache. Choose the list and memory budget from each port's measurements.

## Verification and audio boundary

Compare compensation/legacy, drain off/selected, and DXGI default/selected separately, with and without the diagnostic
Present limiter. Cover synchronous/asynchronous GS, `fps_limit=0`/60, cold/warm transitions, input/menu polling,
movies and long audio playback. Check actual queue depth, field rate, guest progress, underruns/drops and A/V drift.
For UI packs, test absent/invalid manifests, no room list, full precaching, unfinished/failed decode and eviction;
verify first-use HUD/text output and cache use with matching-state captures.

The existing SPU2 FIR uses per-product rounding and saturating SSE-order reduction; both-core stem selection and
the host sinc resampler are already part of the base. This maintenance does not boost wet gain, add an assumed
reverb delay or replace the existing audio core. FIR differential tests do not establish full SPU2 accuracy
or listening/A/V acceptance.

The headless suite covers measured-wait consumption, host deadline shifts without guest-cycle changes, overflow,
idle pacing, option validation and manifest parsing. It does not load DXGI, decode a texture pack, present frames,
measure physical input latency or replace a Windows/MSVC build and per-port visual/listening regression.

# Headless regression tests

From the repository root:

```sh
python -m unittest discover -s tests -v
```

The sync tests use only Python's standard library and temporary directories. They exercise incomplete output,
overlapping paths, dry runs, initial setup, pruning and preservation of hand-written files and timestamps.

The cheat tests require `g++` with UndefinedBehaviorSanitizer. Without `g++`, unittest explicitly skips that class.
They compile the production implementation against headless input stubs; they do not load graphics, audio or game
data. Coverage includes malformed/overflowing numbers, RAM bounds, mirror addresses, repeated writes, BOM and
trailing comments, literal help text, `skip_zero`, and disabling a cheat. Each binary invocation has a five-second
timeout so a regression to a billions-of-stores loop fails promptly.

These tests do not replace the Windows/MSVC full build or in-game validation described in `BUILD_VERIFICATION.md`.

## Per-game particle policies

`test_particle_edges.py` compiles production policy headers and the actual renderer gate/transform and Bridge
Open/Close/reset function bodies against recording GS services, using C++20 and UndefinedBehaviorSanitizer.
Fifteen cases cover no/valid/invalid/mismatched profiles, strict option parsing, force-off alias precedence,
session/environment/capture reset, failed and duplicate opens, repeated game switches, draw/vertex exclusions,
profile-owned sampling parameters and numeric compatibility. A frozen public legacy function from main `05807db`
provides an independent differential baseline for 12,000 synthetic draws, including applied FST/STQ and mip/base-only
paths. The production files supply the tested bodies at build time; the tests do not redefine those implementations.

The fixture deliberately substitutes platform/GS types and host services. It does not compile the complete Windows
GS translation units/DLL, load commercial game data, render pixels or certify visual compatibility. A synthetic
session trace is an operator-observed simulation, not a real-game or GUI test. See
[the separate verification record](../docs/PARTICLE_EDGE_REVIEW_2026-11-04.md).

## Host timing and pack manifests

`test_host_timing.py` compiles production host pacing/options helpers with C++20, threads and UndefinedBehaviorSanitizer.
It covers a genuinely blocked producer versus an already-satisfied wait, consume-once timing, host-only deadline
shifts and overflow, the asynchronous idle wait, absolute host scheduling, bounded numeric/DXGI options and UI manifest
whitespace/BOM/deduplication/path/size limits. It does not exercise a Windows GS worker, DXGI calls, PNG decoding or GPU
uploads. See [HOST_TIMING.md](../docs/HOST_TIMING.md) for the required per-port timing, cache and listening checks.

## Audio and patch parity

Requires `g++` (resampler: C++17 + UndefinedBehaviorSanitizer; SPU2: C++20). The resampler checks output rates,
DC gain, stereo isolation, impulse delay, invalid steps and frequency response. On POSIX, initialization also
runs with a 128 KiB stack, NRVO and compiler folding of trigonometric calls disabled, to detect large temporary
coefficient tables during actual runtime construction. A patch parity check also verifies that the distributable
upstream patch series still matches the vendored source, by reversing/reapplying the ordered patches and comparing
every file byte for byte. The original base patch stays unchanged; later shared changes are carried by a small delta.

On x86 with SSSE3, the production private SPU2 FIR is compared 2,560,000 times against the instruction sequence
in pinned PCSX2 `646df006a4`, `pcsx2/SPU2/ReverbResample.cpp`, SSE path. This verifies per-tap rounding, saturating
lane sums, horizontal reduction, full-scale inputs and all ring positions. AVX2 uses a different accumulation
order and is not the reference for this scalar SSE-compatible implementation.

No game, graphics or audio device is needed. These tests do not replace a Windows/MSVC build or in-game listening.

## Per-game addresses

IPU, MPEG and font tests compile the complete production translation units against recording host services, with
UndefinedBehaviorSanitizer. They require C++20 `g++` and exercise missing/invalid profiles, instruction mismatch,
RAM bounds/aliases, IPU helper continuation, relocated MPEG callbacks/globals, optional movie metadata, repeat-safe
HD hook installation/restoration, font GP arithmetic and font ID bounds. MPEG decoding and font rendering are not
validated by these host stubs. The actual generic runner is also compiled and exercised for configuration errors,
missing CLI values and reapplying validated IPU settings after ELF-load reset.

These checks do not certify a game's ABI or visuals and do not replace a Windows/MSVC build. Existing generated
runners must migrate profile configuration to after `loadELF`; no executable or DLL is deployed by these tests.

Game-level coverage and the failure-to-fix workflow live in [REGRESSION_TESTING.md](../docs/REGRESSION_TESTING.md)
([繁體中文](../docs/REGRESSION_TESTING.zh-TW.md)). A passing headless suite is one gate, not full port acceptance.

## Final portability review checks

MPEG and font tests now also use AddressSanitizer for image/work/header/payload bounds, allocation failure and
corrupt reference/free pointers. The MPEG host stub models production's non-null missing-function trap, so a missing
HD entry cannot masquerade as a native function. Hook ownership, release and partial-install rollback are exercised.
On Linux, non-PIE binaries avoid sanitizer shadow-map collisions. In a ptraced executor without usable `/proc` thread
access, run `ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v`; this disables leak scanning only,
not address/undefined-behaviour checks. These tests do not establish leak freedom or game-rendering acceptance.

`test_build_test_gate.py` compiles the production MiniTest header and feeds its real structured output into
`tools/check_test_results.py`. It checks named historical failures, new failures with the same count, unexpected
exceptions/suites, JSON escaping, zero tests, stale/truncated/duplicate reports, summary/exit inconsistencies and
crashes. `build_all.ps1` requires Python 3.9+ for this gate and requires all configured IOP and GS-cache executables.
The named exception list documents historical Windows observations; it does not revalidate their current causes.

## Neutral host and asset adapters

`test_neutral_base.py` compiles the production pad translation unit, presentation/input policy and HD asset headers,
PNG saver and host identity functions with recording services and UBSan. It covers independent acknowledgements,
menu/source changes, stale tokens, concurrent publication/reads, movie transition combinations, explicit format
selection/reset, relocated HD buffers, sparse-signature collisions, packet arithmetic, split alpha, pitch and write
failures. It requires no commercial game assets or real window/device. Particle lifecycle cases additionally exercise
actual Bridge reset paths and the bounded A/B force-off window. See `docs/NEUTRAL_BASE_MIGRATION.md` for port acceptance.

The cloud CI workflow also compiles the full Windows GS DLL. Read the actual CI result before claiming a Windows
build passed; recording-service tests and Linux syntax checks cover different boundaries. No user-PC build is needed.

## Storage, HLE memory and VAG boundaries

`test_storage_boundaries.py` compiles real VFS/ROM/path/audio translation units and extracted production HLE bodies
with ASan/UBSan. It tests creation versus truncation, permissions, exclusive creation, ROM seek overflow, large host
positions, disc read-only behavior, UTF-8/ISO path rules, backing-allocation boundaries, SIF mirror descriptors and
metadata, and corrupt VAG sizes. Fixtures use temporary synthetic files/buffers and no game assets.

`cmake -S tests -B build/boundaries`, build, then `ctest --test-dir build/boundaries --output-on-failure` runs the same
portable boundary cases. For MSVC use a configured Release build and `-C Release`; the target explicitly undefines
NDEBUG. The GitHub-hosted Windows job must actually run before claiming these platform branches passed.

## Dawn of Dreams reference adapter

`test_dod_reference.py` compiles the actual reference option builder and extracted runner INI/preflight bodies with UBSan and the existing production GS
particle policy. It tests explicit revision/CRC selection, default/force-off, conflicting inputs, independent
UI/CG flags, environment/INI order, reload independence, malformed arguments and synthetic ELF CRC against zlib.
Pack preparation tests use temporary placeholder files and exercise dry run, explicit creation and no-overwrite.
These fixtures do not build the full generated game runner or validate its visuals.

## VFS path encoding

`test_vfs_encoding.py` compiles production VFS/path/ROM and the optional port decoder, using independent Python codec
fixtures for UTF-8, Shift-JIS/CP932, GBK/CP936 and Big5/CP950 common mappings. Temporary files cover read/create/save,
explicit truncation, DBCS trail `0x5c`, invalid input, ambiguous valid UTF-8, ASCII and stateless selection.
POSIX uses iconv with ASan/UBSan; its 27 cases do not establish Windows behavior. One Windows-native-path case is
explicitly skipped on Linux. CMake adds all 28 to the Windows boundary job (56 total with existing cases), using Win32
code-page APIs, the production wide-file path and Release assertions. Read the candidate CI result before claiming
Windows passed. None of these fixtures exercises libmc, IOP filesystem bypasses or real game saves; see
[scope and test matrix](../docs/VFS_PATH_ENCODING_REVIEW_2026-11-06.md).

## Run-time code overlays

`test_overlay_registry.py` compiles the production overlay registry (`ps2_overlays.cpp`) on its own with ASan/UBSan:
empty registry, signature-selected dispatch between images at one load address, a small image inside a larger one,
rejected descriptions and ambiguous signatures, clear, copying of caller storage, and four threads whose lookups use
different RAM contents so the resident cache keeps changing hands. The same six cases run in the Windows boundary job
(`overlay-*`, Release with assertions). Static checks confirm that `loadELF` clears the registry first, that dispatch
consults it only behind the inline range check, and that no shared source registers an image. The `PS2Runtime`
integration and the recompiler's `[overlay]` output are covered by `ps2x_tests` (suites `PS2Overlays`,
`PS2RuntimeOverlays`, `PS2RecompilerOverlays`) in `build_all.ps1`. All images and addresses are synthetic.

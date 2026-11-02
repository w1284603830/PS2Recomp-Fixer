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

## Audio and patch parity

Requires `g++` (resampler: C++17 + UndefinedBehaviorSanitizer; SPU2: C++20). The resampler checks output rates,
DC gain, stereo isolation, impulse delay, invalid steps and frequency response. On POSIX, initialization also
runs with a 128 KiB stack, NRVO and compiler folding of trigonometric calls disabled, to detect large temporary
coefficient tables during actual runtime construction. A patch parity check also verifies that the distributable
upstream diff still matches the vendored source.

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

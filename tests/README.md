# Headless audio regression tests

```sh
python -m unittest discover -s tests -v
```

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

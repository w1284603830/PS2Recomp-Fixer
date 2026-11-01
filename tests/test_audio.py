"""Headless tests for the production host resampler and SPU2 FIR; requires g++."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "audio tests require g++")
class OutputResamplerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "resampler_tests"
        # Exercise runtime construction without NRVO or GCC's compile-time folding of libm calls.
        result = subprocess.run(["g++", "-std=c++17", "-O0", "-fno-elide-constructors", "-Wall", "-Wextra",
                                 "-fno-builtin-sin", "-fno-builtin-cos",
                                 "-fsanitize=undefined", "-fno-sanitize-recover=all",
                                 str(ROOT / "tests/output_resampler_tests.cpp"), "-o", str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, mode, **kwargs):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=20, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rates_match_the_host_drift_range(self):
        self.run_case("rates")

    def test_dc_gain_and_channel_isolation(self):
        self.run_case("dc")

    def test_impulse_delay_and_finite_tail(self):
        self.run_case("delay")

    def test_invalid_steps_are_bounded(self):
        self.run_case("invalid-step")

    def test_passband_and_nyquist_rejection(self):
        self.run_case("spectrum")

    @unittest.skipUnless(os.name == "posix", "stack-limit test uses POSIX resource limits")
    def test_initialization_with_128_kib_stack(self):
        import resource
        def limit_stack():
            _, hard = resource.getrlimit(resource.RLIMIT_STACK)
            resource.setrlimit(resource.RLIMIT_STACK, (128 * 1024, hard))
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        self.run_case("construct", preexec_fn=limit_stack)


@unittest.skipUnless(shutil.which("g++") and platform.machine().lower() in ("x86_64", "amd64", "i686"),
                     "SSE differential test requires x86 g++")
class ReverbFirTests(unittest.TestCase):
    def test_production_fir_matches_pinned_pcsx2_sse(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "fir_tests"
            result = subprocess.run(["g++", "-std=c++20", "-O2", "-pthread",
                                     str(ROOT / "tests/spu2_reverb_fir_tests.cpp"), "-o", str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            if result.returncode == 77:
                self.skipTest("CPU lacks SSSE3")
            self.assertEqual(result.returncode, 0, result.stderr)

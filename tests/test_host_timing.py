"""Exercise production host timing/options helpers without a graphics/audio device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "third_party/src/PS2Recomp/ps2xRuntime"
GS = ROOT / "third_party/src/pcsx2gs/pcsx2/pcsx2"


@unittest.skipUnless(shutil.which("g++"), "host timing tests require g++")
class HostTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "host_timing_tests"
        result = subprocess.run([
            "g++", "-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra",
            "-fsanitize=undefined", "-fno-sanitize-recover=all",
            "-I", str(RUNTIME / "include"), "-I", str(GS),
            str(ROOT / "tests/host_timing_tests.cpp"),
            str(RUNTIME / "src/lib/dod_host_pacer.cpp"), "-o", str(cls.exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_blocked_producer_time_is_consumed_once(self): self.run_case("wait")
    def test_host_shifts_preserve_guest_cycles_and_event_order(self): self.run_case("deadlines")
    def test_extreme_durations_saturate(self): self.run_case("overflow")
    def test_drain_and_present_options_are_bounded(self): self.run_case("options")
    def test_async_idle_wait_and_absolute_host_schedule(self): self.run_case("pacer")
    def test_dxgi_options_leave_invalid_input_unselected(self): self.run_case("latency")
    def test_manifest_trim_dedup_and_path_rejection(self): self.run_case("manifest")
    def test_manifest_queue_and_input_limits(self): self.run_case("manifest-limits")

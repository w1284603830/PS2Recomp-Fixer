"""Production overlay registry (ps2_overlays.cpp) under ASan/UBSan and threads; synthetic addresses only."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "third_party/src/PS2Recomp/ps2xRuntime"
CASES = ("empty", "dispatch", "reject", "clear", "copy", "threads")


@unittest.skipUnless(shutil.which("g++"), "requires g++ with ASan/UBSan")
class OverlayRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "overlay_registry"
        command = ["g++", "-std=c++20", "-g", "-O1", "-pthread", "-Wall", "-Wextra",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if sys.platform.startswith("linux"):
            command += ["-fno-pie", "-no-pie"]
        command += ["-I", str(RT / "include"), str(ROOT / "tests/overlay_registry_tests.cpp"),
                    str(RT / "src/lib/ps2_overlays.cpp"), "-o", str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=60,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        self.assertEqual(result.returncode, 0, result.stderr)


for _case in CASES:
    def _test(self, case=_case):
        self.run_case(case)
    setattr(OverlayRegistryTests, "test_" + _case, _test)


class OverlayRuntimeWiringTests(unittest.TestCase):
    """The registry stays opt-in: cleared by loadELF and consulted only behind the inline range check."""

    def test_load_elf_clears_before_anything_else(self):
        source = (RT / "src/lib/ps2_runtime.cpp").read_text(encoding="utf-8")
        body = source[source.index("bool PS2Runtime::loadELF("):]
        body = body[:body.index("\n}\n")]
        first_statement = [line.strip() for line in body.splitlines()[2:] if line.strip() and
                           not line.strip().startswith("//")][0]
        self.assertEqual(first_statement, "ps2_overlays::clear();")

    def test_dispatch_paths_are_guarded_by_may_cover(self):
        source = (RT / "src/lib/ps2_runtime.cpp").read_text(encoding="utf-8")
        for function in ("bool PS2Runtime::hasFunction(", "PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction("):
            body = source[source.index(function):]
            body = body[:body.index("\n}\n")]
            self.assertIn("ps2_overlays::mayCover(address)", body, function)
            self.assertLess(body.index("ps2_overlays::mayCover(address)"), body.index("ps2_overlays::resolve("), function)

    def test_no_default_overlay_registrations(self):
        # Shared sources declare no image: every registration comes from a port's generated overlay sources.
        offenders = []
        for path in (RT / "src").rglob("*.cpp"):
            text = path.read_text(encoding="utf-8", errors="replace")
            if "CatalogRegistrar " in text and path.name != "ps2_overlays.cpp":
                offenders.append(str(path.relative_to(ROOT)))
            if "registerImage(" in text and path.name != "ps2_overlays.cpp":
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()

"""Optional F3 trainer / F2 cheat-menu switches: python -m unittest discover -s tests -v (requires g++).

Both tools are OFF by default. These tests compile the production trainer panel against recording GS-menu services,
the default (disabled) entry points, and the CMake / build-script switches that select between them."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "third_party/src/PS2Recomp/ps2xRuntime"


class SwitchDefaultsTests(unittest.TestCase):
    """Static checks; no compiler needed."""

    def test_cmake_options_default_off(self):
        text = (RUNTIME / "CMakeLists.txt").read_text(encoding="utf-8")
        for option in ("PS2X_ENABLE_CHEAT_MENU", "PS2X_ENABLE_TRAINER"):
            match = re.search(r'option\(%s\s+"[^"]*"\s+(\w+)\)' % option, text)
            self.assertIsNotNone(match, option)
            self.assertEqual(match.group(1), "OFF", f"{option} must default to OFF")
        self.assertIn("src/lib/ps2_cheats_disabled.cpp", text)
        self.assertIn("src/lib/ps2_trainer_disabled.cpp", text)
        self.assertIn("PS2X_TRAINER=$<BOOL:${PS2X_ENABLE_TRAINER}>", text)
        self.assertIn("PS2X_CHEAT_MENU=$<BOOL:${PS2X_ENABLE_CHEAT_MENU}>", text)
        sources = text[text.index("add_library(ps2_runtime STATIC"):]
        sources = sources[:sources.index(")")]
        self.assertNotIn("src/lib/ps2_cheats.cpp", sources, "the enabled menu must be selected by the option only")
        self.assertNotIn("src/lib/ps2_trainer.cpp", sources, "the enabled trainer must be selected by the option only")

    def test_build_script_switches_default_off(self):
        text = (ROOT / "build_all.ps1").read_text(encoding="utf-8")
        self.assertTrue(all(ord(c) < 128 for c in text), "PowerShell 5.1 reads BOM-less scripts as ANSI: keep ASCII")
        self.assertIn("[switch]$CheatMenu", text)
        self.assertIn("[switch]$Trainer", text)
        self.assertIn("-DPS2X_ENABLE_CHEAT_MENU=$(if ($CheatMenu) { 'ON' } else { 'OFF' })", text)
        self.assertIn("-DPS2X_ENABLE_TRAINER=$(if ($Trainer) { 'ON' } else { 'OFF' })", text)


@unittest.skipUnless(shutil.which("g++"), "headless C++ tests require g++")
class TrainerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "trainer_tests"
        cls.disabled = Path(cls.temp.name) / "trainer_disabled_tests"
        common = ["g++", "-std=c++17", "-Wall", "-Wextra", "-pthread", "-fsanitize=undefined",
                  "-fno-sanitize-recover=all", "-I", str(ROOT / "tests/stubs/trainer"), "-I", str(RUNTIME / "include")]
        for source, out, defines in (("ps2_trainer_tests.cpp", cls.exe, ["-DPS2X_TRAINER=1"]),
                                     ("ps2_trainer_disabled_tests.cpp", cls.disabled, [])):
            result = subprocess.run(common + defines + [str(ROOT / "tests" / source), "-o", str(out)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)

    def run_case(self, mode, **env):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=5,
                                env={**os.environ, **env})
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_registry_and_invalid_ids(self):
        self.run_case("registry")

    def test_entry_limit(self):
        self.run_case("limit")

    def test_test_environment_selects_switches(self):
        self.run_case("env-list", PS2_TRAINER="1")
        self.run_case("env-all", PS2_TRAINER="all")
        result = self.run_case("env-invalid", PS2_TRAINER="x")
        self.assertIn("PS2_TRAINER", result.stderr)

    def test_hotkeys_and_shared_menu_slot(self):
        self.run_case("keys")

    def test_scheduled_open(self):
        self.run_case("auto-open", PS2_TRAINER_MENU_AT="5")

    def test_panel_controls(self):
        self.run_case("draw")

    def test_default_build_is_constant_off(self):
        result = subprocess.run([str(self.disabled)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_source_for_the_option_does_not_compile(self):
        for source, define in (("ps2_trainer.cpp", "-DPS2X_TRAINER=0"), ("ps2_trainer_disabled.cpp", "-DPS2X_TRAINER=1")):
            with self.subTest(source=source):
                result = subprocess.run(["g++", "-std=c++17", "-fsyntax-only", define,
                                         "-I", str(ROOT / "tests/stubs/trainer"), "-I", str(RUNTIME / "include"),
                                         str(RUNTIME / "src/lib" / source)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("F3 trainer", result.stderr, "the #error must name the mismatched option")


if __name__ == "__main__":
    unittest.main()

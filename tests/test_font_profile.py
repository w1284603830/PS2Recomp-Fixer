"""Compile the full production font HLE with recording host services."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "third_party/src/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs"


@unittest.skipUnless(shutil.which("g++"), "headless font tests require g++")
class FontProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        for name in ("Font.cpp", "Font.h", "GuestAddress.h"):
            shutil.copyfile(LIB / name, directory / name)
        shutil.copyfile(ROOT / "tests/stubs/mpeg/Common.h", directory / "Common.h")
        (directory / "ps2_stubs.h").write_text('#pragma once\n#include "Common.h"\n')
        cls.exe = directory / "font_tests"
        result = subprocess.run(["g++", "-std=c++20", "-O1", "-fsanitize=address,undefined", *(["-fno-pie", "-no-pie"] if sys.platform.startswith("linux") else []),
                                 "-fno-sanitize-recover=all", "-I", str(directory),
                                 str(ROOT / "tests/font_profile_tests.cpp"), "-o", str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_game_specific_defaults(self):
        self.run_case("unconfigured")

    def test_wrong_instruction_rejects_every_entry(self):
        self.run_case("mismatch")

    def test_invalid_profiles_disable_every_entry(self):
        self.run_case("invalid")

    def test_relocated_globals_and_reload_reset(self):
        self.run_case("relocated")

    def test_font_ids_cannot_escape_the_configured_table(self):
        self.run_case("font-id")

    def test_font_header_and_payload_bounds(self):
        self.run_case("source-bounds")

    def test_allocation_failures_preserve_font_and_release_partial_allocations(self):
        self.run_case("allocation")

    def test_loaded_single_and_double_byte_fonts_close_owned_allocations(self):
        self.run_case("load-close")

    def test_corrupt_close_pointers_cannot_escape_ram(self):
        self.run_case("close-bounds")

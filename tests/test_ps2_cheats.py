"""Headless production-parser tests: python -m unittest discover -s tests -v (requires g++)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "headless C++ tests require g++")
class CheatsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "cheats_tests"
        result = subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=undefined",
            "-fno-sanitize-recover=all", "-I", str(ROOT / "tests/stubs"),
            "-I", str(ROOT / "third_party/src/PS2Recomp/ps2xRuntime/include"),
            str(ROOT / "tests/ps2_cheats_tests.cpp"), "-o", str(cls.exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, *args, env=None):
        result = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=5, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_numbers_are_rejected(self):
        for line in (
            "0x100000000,u8,1", "-1,u8,1", "+1,u8,1", "0x,u8,1",
            "0,u8,256", "0,u16,65536", "0,u32,4294967296", "0,u32,-1",
            "0,u32,18446744073709551616", "0,u8,1,count=", "0,u8,1,count=0",
            "0,u8,1,count=2junk", "0,u8,1,count=-1", "0,u8,1,count=4294967296",
            "0,u8,1,stride=", "0,u8,1,stride=8junk", "0,u8,1,stride=-1",
            "0,u8,1,stride=4294967296",
        ):
            with self.subTest(line=line):
                self.run_case("reject", line)

    def test_leading_zero_numbers_are_decimal(self):
        self.run_case("decimal")

    def test_same_address_repeats_are_collapsed(self):
        self.run_case("repeat")

    def test_ram_boundaries_mirrors_and_parser_reuse(self):
        self.run_case("bounds")

    def test_documented_comments_and_actual_ram_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "cheats.ini"
            config.write_text(
                "\ufeff[section:Player] ; panel title\n"
                "[Health #1; reserve] # checkbox\n"
                "help=Keep # and ; in help.\n"
                "note=Keep # and ; in notes.\n"
                "write=0x1ffffff,u8,255 ; last byte of RAM\n"
                "[Inventory]\n"
                "write=16,u8,99,count=6,stride=1,skip_zero # empty slots remain empty\n",
                encoding="utf-8",
            )
            self.run_case("file", env={**os.environ, "PS2_CHEATS_FILE": str(config)})

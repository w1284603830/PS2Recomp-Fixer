"""Compile the production IPU initializer and generic runner against recording host stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = ROOT / "tests/stubs/ipu"
LIB = ROOT / "third_party/src/PS2Recomp/ps2xRuntime/src/lib"


@unittest.skipUnless(shutil.which("g++"), "headless C++ tests require g++")
class IpuInitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        ipu = cls.directory / "lib/Kernel/Stubs"
        runner = cls.directory / "runner"
        gs = cls.directory / "lib/gs"
        for path in (ipu, runner, gs):
            path.mkdir(parents=True)
        for name in ("IPU.cpp", "IPU.h", "GuestAddress.h"):
            shutil.copyfile(LIB / "Kernel/Stubs" / name, ipu / name)
        shutil.copyfile(LIB / "gs/dod_pcsx2_gs.h", gs / "dod_pcsx2_gs.h")
        shutil.copyfile(ROOT / "examples/generic_runner/main.cpp", runner / "main.cpp")
        cls.exe = cls.directory / "ipu_tests"
        cls.runner = cls.directory / "runner_tests"
        common = ["g++", "-std=c++20", "-Wall", "-Wextra", "-fsanitize=undefined",
                  "-fno-sanitize-recover=all", "-I", str(STUBS), "-I", str(ipu)]
        for files, output in (([ROOT / "tests/ipu_init_tests.cpp"], cls.exe),
                              ([runner / "main.cpp", ipu / "IPU.cpp", ROOT / "tests/generic_runner_stubs.cpp"], cls.runner)):
            result = subprocess.run([*common, *map(str, files), "-o", str(output)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_game_specific_defaults(self):
        self.run_case("unconfigured")

    def test_bad_table_ranges_and_alignment_do_not_touch_guest_state(self):
        for mode in ("invalid-iq", "invalid-vq", "unaligned-iq", "unaligned-vq", "invalid-helper", "unaligned-helper", "invalid-segment"):
            with self.subTest(mode=mode):
                self.run_case(mode)

    def test_reloading_clears_the_previous_game_config(self):
        self.run_case("reset")

    def test_direct_dma_initialization(self):
        self.run_case("direct")

    def test_missing_guest_helper_falls_back_to_direct_dma(self):
        self.run_case("missing-helper")

    def test_guest_helper_completes_initialization_once(self):
        self.run_case("helper")

    def test_last_valid_ram_ranges(self):
        self.run_case("last-byte")

    def test_mirrored_ram_addresses(self):
        self.run_case("mirrors")

    def test_initialization_failures_do_not_load_tables(self):
        for mode in ("memory-failure", "sync-failure", "null-runtime"):
            with self.subTest(mode=mode):
                self.run_case(mode)

    def run_runner(self, *args):
        return subprocess.run([str(self.runner), *args], capture_output=True, text=True, timeout=5)

    def test_runner_rejects_unknown_and_missing_options(self):
        for args in (("--iso",), ("--ipu_iq",), ("--typo", "1"), ("--", "1")):
            with self.subTest(args=args):
                result = self.run_runner(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("[test-runtime] initialize", result.stderr)

    def test_runner_validates_ipu_config_before_initializing_host(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, iso = (Path(directory) / name for name in ("boot.elf", "game.iso"))
            elf.touch()
            iso.touch()
            base = ("--elf", str(elf), "--iso", str(iso))
            for args in (("--ipu_iq", "4096"), ("--ipu_vq", "8192"),
                         ("--ipu_iq", "-1", "--ipu_vq", "8192"),
                         ("--ipu_iq", "4294967296", "--ipu_vq", "8192"),
                         ("--ipu_iq", "4096junk", "--ipu_vq", "8192"),
                         ("--ipu_iq", "4097", "--ipu_vq", "8192"),
                         ("--ipu_iq", "0x1ffffc0", "--ipu_vq", "8192"),
                         ("--ipu_iq", "4096", "--ipu_vq", "8192", "--ipu_d4_chcr", "0x3001"),
                         ("--gparam", "0x100000000"), ("--gparam", "0x10002010"),
                         ("--gparam", "0x1fffff8"), ("--gparam", "0x1001")):
                with self.subTest(args=args):
                    result = self.run_runner(*base, *args)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertNotIn("[test-runtime] initialize", result.stderr)
            result = self.run_runner(*base, "--ipu_iq", "0x1000", "--ipu_vq", "0008192", "--ipu_d4_chcr", "0")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("[test-runtime] initialize", result.stderr)
            self.assertIn("[test-runtime] ipu=1", result.stderr)
            result = self.run_runner(*base)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("[test-runtime] ipu=0", result.stderr)

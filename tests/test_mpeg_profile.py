"""Compile the full production MPEG translation unit with recording host services and no codec/device."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "third_party/src/PS2Recomp/ps2xRuntime/src/lib"


@unittest.skipUnless(shutil.which("g++"), "headless MPEG tests require g++")
class MpegProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        stubs = directory / "lib/Kernel/Stubs"
        gs = directory / "lib/gs"
        for path in (stubs, gs, directory / "runtime", directory / "Syscalls/Helpers"):
            path.mkdir(parents=True)
        for name in ("MPEG.cpp", "MPEG.h", "MpegGameProfile.h", "GuestAddress.h"):
            shutil.copyfile(LIB / "Kernel/Stubs" / name, stubs / name)
        shutil.copyfile(LIB / "dod_mpeg_dma_ingress.h", directory / "lib/dod_mpeg_dma_ingress.h")
        for name in ("Common.h", "dod_hd_cg.h"):
            shutil.copyfile(ROOT / "tests/stubs/mpeg" / name, stubs / name)
        shutil.copyfile(ROOT / "tests/stubs/mpeg/dod_pcsx2_gs.h", gs / "dod_pcsx2_gs.h")
        for name in ("ps2_stubs.h", "runtime/ee_scheduler.h", "Syscalls/Helpers/State.h"):
            (directory / name).write_text('#pragma once\n#include "Common.h"\n')
        cls.exe = directory / "mpeg_tests"
        result = subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-DPS2X_HAS_FFMPEG=0",
                                 "-fsanitize=address,undefined", *(["-fno-pie", "-no-pie"] if sys.platform.startswith("linux") else []), "-fno-sanitize-recover=all", "-Wall", "-Wextra",
                                 "-I", str(stubs), "-I", str(directory),
                                 str(ROOT / "tests/mpeg_profile_tests.cpp"), "-o", str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unconfigured_hle_does_not_touch_guest_state(self):
        self.run_case("unconfigured")

    def test_instruction_mismatch_does_not_fall_back_to_legacy(self):
        self.run_case("mismatch")

    def test_relocated_work19a0_callbacks(self):
        self.run_case("work19a0")

    def test_relocated_legacy_globals_and_callbacks(self):
        self.run_case("legacy")

    def test_invalid_profiles_disable_hle(self):
        self.run_case("invalid")

    def test_hd_hooks_install_once_and_restore_on_reload(self):
        self.run_case("hooks")

    def test_incomplete_hooks_leave_native_functions_intact(self):
        self.run_case("incomplete-hooks")

    def test_movie_lookup_uses_profile_and_checks_ranges(self):
        self.run_case("movie")

    def test_movie_field_workaround_requires_explicit_profile_and_instruction(self):
        self.run_case("field")

    def test_wrong_runtime_cannot_discard_installed_native_hooks(self):
        self.run_case("hook-reset-owner")

    def test_runtime_release_restores_native_hooks(self):
        self.run_case("hook-release")

    def test_partial_hook_installation_rolls_back(self):
        self.run_case("hook-rollback")

    def test_image_and_word_writes_do_not_cross_ram(self):
        self.run_case("buffers")

    def test_invalid_guest_objects_and_work_blocks_do_not_touch_memory(self):
        self.run_case("guest-work")

    def test_legacy_allocation_failure_leaves_guest_state_intact(self):
        self.run_case("legacy-allocation")

    def test_bad_picture_destination_preserves_frame_and_zero_work_preserves_kernel(self):
        self.run_case("legacy-picture")

    def test_reference_pointer_arithmetic_cannot_wrap_to_kernel(self):
        self.run_case("legacy-reference")

    def test_work19a0_rejects_bad_images_without_consuming_the_frame(self):
        self.run_case("dod-picture")

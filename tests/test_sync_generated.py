"""Exercise synchronization as a CLI against temporary runtime trees."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/sync_generated.py"


class SyncGeneratedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "generated"
        self.runtime = self.root / "runtime"
        self.runner = self.runtime / "src/runner"
        self.include = self.runtime / "include"
        for directory in (self.source, self.runner, self.include):
            directory.mkdir(parents=True)
        for name in ("register_functions.cpp", "ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
            (self.source / name).write_text("// generated\n")
        (self.runner / "FUN_0x00001000.cpp").write_text("// obsolete\n")
        (self.runner / "main.cpp").write_text("// custom entry point\n")
        (self.runner / "custom.cpp").write_text("// handwritten\n")

    def run_sync(self, *options, source=None, runtime=None):
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), *options, str(source or self.source), str(runtime or self.runtime)],
            capture_output=True, text=True, timeout=5,
        )

    def snapshot(self):
        return {str(p.relative_to(self.runtime)): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.runtime.rglob("*") if p.is_file()}

    def test_valid_sync_prunes_only_generated_files_and_preserves_timestamps(self):
        (self.source / "FUN_0x00002000.cpp").write_text("// new\n")
        result = self.run_sync()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.runner / "FUN_0x00001000.cpp").exists())
        for name in ("main.cpp", "custom.cpp", "register_functions.cpp", "FUN_0x00002000.cpp"):
            self.assertTrue((self.runner / name).is_file(), name)
        before = self.snapshot()
        self.assertEqual(self.run_sync().returncode, 0)
        self.assertEqual(before, self.snapshot())

    def test_empty_output_does_not_delete_existing_code(self):
        for path in self.source.iterdir():
            path.unlink()
        before = self.snapshot()
        result = self.run_sync()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete", result.stderr)
        self.assertEqual(before, self.snapshot())

    def test_incomplete_output_does_not_partially_copy(self):
        (self.source / "ps2_recompiled_stubs.h").unlink()
        (self.source / "FUN_0x00002000.cpp").write_text("// new\n")
        before = self.snapshot()
        self.assertNotEqual(self.run_sync().returncode, 0)
        self.assertEqual(before, self.snapshot())

    def test_main_cpp_is_rejected_before_any_changes(self):
        (self.source / "main.cpp").write_text("// wrong\n")
        before = self.snapshot()
        self.assertNotEqual(self.run_sync().returncode, 0)
        self.assertEqual(before, self.snapshot())

    def test_non_file_output_is_rejected_before_any_changes(self):
        (self.source / "oops.h").mkdir()
        before = self.snapshot()
        self.assertNotEqual(self.run_sync().returncode, 0)
        self.assertEqual(before, self.snapshot())

    def test_dry_run_changes_nothing(self):
        before = self.snapshot()
        result = self.run_sync("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[dry run]", result.stdout)
        self.assertEqual(before, self.snapshot())

    def test_fresh_runtime_can_be_created_and_dry_run_does_not_create_it(self):
        fresh = self.root / "fresh"
        self.assertEqual(self.run_sync("--dry-run", runtime=fresh).returncode, 0)
        self.assertFalse(fresh.exists())
        result = self.run_sync(runtime=fresh)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((fresh / "src/runner/register_functions.cpp").is_file())
        self.assertTrue((fresh / "include/ps2_recompiled_stubs.h").is_file())

    def test_overlapping_directories_are_rejected(self):
        before = self.snapshot()
        for source in (self.runner, self.include, self.runtime):
            with self.subTest(source=source):
                result = self.run_sync(source=source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("overlap", result.stderr)
                self.assertEqual(before, self.snapshot())

    def test_unknown_flags_are_rejected(self):
        before = self.snapshot()
        self.assertNotEqual(self.run_sync("--dry-rnu").returncode, 0)
        self.assertEqual(before, self.snapshot())

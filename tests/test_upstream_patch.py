"""The distributable patch must reproduce the checked-in vendored source."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("git"), "patch parity test requires git")
class UpstreamPatchTests(unittest.TestCase):
    def test_patch_matches_vendored_source(self):
        patch_dir = ROOT / "patches"
        names = [line.strip() for line in (patch_dir / "PS2Recomp_upstream_series.txt").read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertTrue(names, "an empty patch series cannot validate source parity")
        self.assertEqual(len(names), len(set(names)), "duplicate patch in series")
        patches = [patch_dir / name for name in names]
        for patch in patches:
            self.assertEqual(patch.parent, patch_dir)
            self.assertTrue(patch.is_file(), f"missing patch: {patch.name}")
        vendor = ROOT / "third_party/src/PS2Recomp"
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory) / "source"
            shutil.copytree(vendor, tree)
            def apply(patch, reverse=False):
                command = ["git", "apply", "--unidiff-zero"]
                if reverse:
                    command.append("--reverse")
                result = subprocess.run(command + [str(patch)], cwd=tree, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, f"{patch.name}: {result.stderr}")
            for patch in reversed(patches):
                apply(patch, reverse=True)
            for patch in patches:
                apply(patch)
            expected = {p.relative_to(vendor) for p in vendor.rglob("*") if p.is_file()}
            actual = {p.relative_to(tree) for p in tree.rglob("*") if p.is_file()}
            self.assertEqual(actual, expected)
            for path in sorted(expected):
                self.assertEqual((tree / path).read_bytes(), (vendor / path).read_bytes(), str(path))

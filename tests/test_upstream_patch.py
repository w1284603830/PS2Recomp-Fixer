"""The distributable patch must reproduce the checked-in vendored source."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("git"), "patch parity test requires git")
class UpstreamPatchTests(unittest.TestCase):
    def test_patch_matches_vendored_source(self):
        result = subprocess.run([
            "git", "apply", "--reverse", "--check", "--directory=third_party/src/PS2Recomp",
            "patches/PS2Recomp_vs_upstream_eb8688b.diff",
        ], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

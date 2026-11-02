"""Exercise the production MiniTest reporter and build validator together, without Windows/game dependencies."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "MiniTest integration requires g++")
class BuildTestGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.exe = cls.directory / "mini_tests"
        result = subprocess.run(["g++", "-std=c++17", "-I",
                                 str(ROOT / "third_party/src/PS2Recomp/ps2xTest/include"),
                                 str(ROOT / "tests/minitest_report_fixture.cpp"), "-o", str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.exceptions = cls.directory / "exceptions.json"
        cls.exceptions.write_text(json.dumps({"failures": [{
            "suite": 'Smoke"\\\n測試', "name": "known failure", "status": "failed", "reason": "fixture"
        }]}), encoding="utf-8")

    def output(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, encoding="utf-8", timeout=5)
        return result.stdout, result.returncode

    def validate(self, log, code, expected):
        path = self.directory / "run.log"
        path.write_text(log, encoding="utf-8-sig") # Same BOM as Windows PowerShell Out-File -Encoding utf8.
        result = subprocess.run([sys.executable, str(ROOT / "tools/check_test_results.py"), str(path),
                                 "--exit-code", str(code), "--exceptions", str(self.exceptions)],
                                capture_output=True, text=True, encoding="utf-8", timeout=5)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_passing_cases_and_escaped_names(self):
        self.validate(*self.output("pass"), 0)

    def test_named_historical_failure_is_accepted(self):
        self.validate(*self.output("known"), 0)

    def test_new_failure_is_rejected_even_with_the_same_count(self):
        self.validate(*self.output("new"), 1)

    def test_new_exception_in_a_known_failed_case_is_rejected(self):
        self.validate(*self.output("error"), 1)

    def test_failure_in_another_suite_is_rejected(self):
        self.validate(*self.output("other-suite"), 1)

    def test_zero_tests_is_rejected(self):
        self.validate(*self.output("zero"), 1)

    def test_truncated_and_stale_reports_are_rejected(self):
        log, code = self.output("pass")
        self.validate(log.split("[TestSummary]")[0], code, 1)
        self.validate("Total Tests: 2\nPassed: 2\nFailed: 0\n", 0, 1)

    def test_crash_or_wrong_exit_is_rejected(self):
        log, _ = self.output("pass")
        self.validate(log, -11, 1)
        log, _ = self.output("known")
        self.validate(log, 0, 1)

    def test_duplicate_cases_and_inconsistent_summary_are_rejected(self):
        log, code = self.output("pass")
        record = next(line for line in log.splitlines() if line.startswith("[TestResult]"))
        self.validate(record + "\n" + log, code, 1)
        self.validate(log.replace('"total":12', '"total":99'), code, 1)

    def test_malformed_record_is_rejected(self):
        self.validate('[TestResult] {bad json}\n', 0, 1)

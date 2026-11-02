"""Validate MiniTest's structured log and named historical exceptions; standard library only."""
import argparse
import json
from pathlib import Path
import sys


def validate(log, exit_code, exceptions):
    allowed = {}
    for entry in exceptions["failures"]:
        key = (entry["suite"], entry["name"])
        if key in allowed or entry["status"] not in ("failed", "error") or not entry["reason"]:
            raise ValueError("invalid or duplicate known-failure entry")
        allowed[key] = entry["status"]
    results = {}
    summary = None
    for line in log.splitlines():
        if line.startswith("[TestResult] "):
            if summary is not None:
                raise ValueError("test result after final summary")
            result = json.loads(line[len("[TestResult] "):])
            key = (result["suite"], result["name"])
            if not all(isinstance(part, str) and part for part in key) or key in results:
                raise ValueError("invalid or duplicate test identity")
            if result["status"] not in ("passed", "failed", "error"):
                raise ValueError("unknown test result")
            results[key] = result["status"]
        elif line.startswith("[TestSummary] "):
            if summary is not None:
                raise ValueError("duplicate final summary")
            summary = json.loads(line[len("[TestSummary] "):])
    if not results or summary is None:
        raise ValueError("missing test results/summary; zero tests or incomplete/stale executable")
    failures = {key: status for key, status in results.items() if status != "passed"}
    expected = {"total": len(results), "passed": len(results) - len(failures), "failed": len(failures)}
    if any(type(summary.get(key)) is not int or summary[key] != value for key, value in expected.items()):
        raise ValueError("case results do not match summary")
    native_code = len(failures) if sys.platform == "win32" else len(failures) % 256
    if exit_code != native_code:
        raise ValueError(f"native exit {exit_code} does not match {len(failures)} reported failures")
    unexpected = [(suite, name, status) for (suite, name), status in failures.items()
                  if allowed.get((suite, name)) != status]
    if unexpected:
        raise ValueError("unexpected test failure(s): " + "; ".join(f"{s}/{n} [{status}]" for s, n, status in unexpected))
    return expected, [key for key in allowed if results.get(key) == "passed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--exit-code", type=int, required=True)
    parser.add_argument("--exceptions", type=Path,
                        default=Path(__file__).with_name("known_test_failures.json"))
    args = parser.parse_args()
    try:
        counts, recovered = validate(args.log.read_text(encoding="utf-8-sig"), args.exit_code,
                                     json.loads(args.exceptions.read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"[test-gate] FAIL: {error}", file=sys.stderr)
        return 1
    print(f"[test-gate] {counts['total']} cases, {counts['passed']} passed, {counts['failed']} named exceptions")
    if recovered:
        print("[test-gate] passing historical exceptions (review for removal): " +
              "; ".join(f"{suite}/{name}" for suite, name in recovered))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

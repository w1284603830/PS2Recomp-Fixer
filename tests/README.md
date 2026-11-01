# Focused regression tests

From the repository root:

```sh
python -m unittest discover -s tests -v
```

The sync tests use only Python's standard library and temporary directories. They exercise incomplete output,
overlapping paths, dry runs, initial setup, pruning and preservation of hand-written files and timestamps.

The cheat tests require `g++` with UndefinedBehaviorSanitizer. Without `g++`, unittest explicitly skips that class.
They compile the production implementation against headless input stubs; they do not load graphics, audio or game
data. Coverage includes malformed/overflowing numbers, RAM bounds, mirror addresses, repeated writes, BOM and
trailing comments, literal help text, `skip_zero`, and disabling a cheat. Each binary invocation has a five-second
timeout so a regression to a billions-of-stores loop fails promptly.

These tests do not replace the Windows/MSVC full build or in-game validation described in `BUILD_VERIFICATION.md`.

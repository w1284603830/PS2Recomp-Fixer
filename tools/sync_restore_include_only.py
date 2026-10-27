"""After sync_generated_added.py: put back the files whose only change is the added
`#include <ps2_recompiled_functions_added.h>` line.

The sync adds that include to every file it copies, and when the regenerated set only gains functions reached through
the function table (no generated file names them), every file differs from the runtime's copy by that one line and the
whole runner recompiles. This restores those files from the sync's backup (shutil.copy2, so the old timestamps come
back and the build skips them). Files with other changes, new files and register_functions.cpp stay as synced.

Usage: python -I sync_restore_include_only.py <runner dir> <backup dir>
"""
import shutil
import sys
from pathlib import Path

INCLUDE = "#include <ps2_recompiled_functions_added.h>"

runner, backup = Path(sys.argv[1]), Path(sys.argv[2])
restored = kept = 0
kept_names = []
for saved in sorted(backup.glob("*.cpp")):
    current = runner / saved.name
    if not current.exists():
        continue
    new_lines = current.read_text(encoding="utf-8", errors="surrogateescape").splitlines(keepends=True)
    old_lines = saved.read_text(encoding="utf-8", errors="surrogateescape").splitlines(keepends=True)
    stripped = [l for l in new_lines if l.strip() != INCLUDE]
    if stripped == old_lines and len(new_lines) == len(old_lines) + 1:
        shutil.copy2(saved, current)
        restored += 1
    else:
        kept += 1
        kept_names.append(saved.name)
print(f"restored {restored} include-only files, kept {kept} changed files")
for name in kept_names[:60]:
    print("  kept", name)

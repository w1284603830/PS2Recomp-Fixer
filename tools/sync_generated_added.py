"""Sync regenerated code that adds functions without touching the headers every generated file includes.

New function declarations go to include/ps2_recompiled_functions_added.h; every copied (changed or new) .cpp gets an
#include of it right after <ps2_recompiled_functions.h>. ps2_recompiled_functions.h and ps2_recompiled_stubs.h keep
their current content as long as the new versions only add declarations (functions) or only remove declarations
(stubs). Replaced files are backed up; nothing is deleted.
Usage: sync_generated_added.py <generated dir> <ps2xRuntime dir> <backup dir> <report.json>"""
import json
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

source, runtime, backup, report = (Path(a) for a in sys.argv[1:5])
if report.exists():
    raise SystemExit(f'Refusing to overwrite report: {report}')
runner = runtime / 'src/runner'
include = runtime / 'include'
old_names = {p.name for p in runner.glob('*.cpp') if p.name != 'main.cpp' and not p.name.startswith('zz_dod_added_functions_')}
new_names = {p.name for p in source.glob('*.cpp')}
if old_names - new_names:
    raise SystemExit(f'Generated set lost files: {sorted(old_names - new_names)[:20]}')

functions_old = set((include / 'ps2_recompiled_functions.h').read_text().splitlines())
functions_new = (source / 'ps2_recompiled_functions.h').read_text().splitlines()
added_decls = [l for l in functions_new if l not in functions_old]
if set(functions_old) - set(functions_new):
    raise SystemExit('ps2_recompiled_functions.h lost lines; review required')
stubs_old = set((include / 'ps2_recompiled_stubs.h').read_text().splitlines())
stubs_new = set((source / 'ps2_recompiled_stubs.h').read_text().splitlines())
stub_added = sorted(stubs_new - stubs_old)
added_decls += [l for l in stub_added if l.startswith('void ')]

INCLUDE_LINE = '#include <ps2_recompiled_functions_added.h>'
added_header = include / 'ps2_recompiled_functions_added.h'
header_text = '#pragma once\n// [DoD] Declarations of functions added after the generated headers were frozen (sync_generated_added.py).\n' \
              '#include <cstdint>\nstruct R5900Context;\nclass PS2Runtime;\n' + '\n'.join(added_decls) + '\n'


def transformed(path):
    text = path.read_text()
    anchor = '#include <ps2_recompiled_functions.h>'
    if added_decls and anchor in text and INCLUDE_LINE not in text:
        text = text.replace(anchor, anchor + '\n' + INCLUDE_LINE, 1)
    return text


def differs(path):
    dest = runner / path.name
    if not dest.exists():
        return True
    return dest.read_text() != transformed(path)


files = sorted(source.glob('*.cpp'))
with ThreadPoolExecutor(max_workers=16) as pool:
    flags = list(pool.map(differs, files))
todo = [p for p, f in zip(files, flags) if f]
backup.mkdir(parents=True, exist_ok=True)
changed, added = [], []
# New functions go into a few combined files whose names sort last: the unity batches (32 files in name order) of
# the existing files stay as they are, so only the batches with changed files are compiled again.
COMBINED = 4
combined = [[] for _ in range(COMBINED)]
for path in todo:
    dest = runner / path.name
    if dest.exists():
        saved = backup / path.name
        if saved.exists():
            raise SystemExit(f'Refusing to overwrite backup: {saved}')
        shutil.copy2(dest, saved)
        changed.append(path.name)
        dest.write_text(transformed(path))
    else:
        added.append(path.name)
        combined[len(added) % COMBINED].append(f'// ---- {path.name}\n' + transformed(path))
for i, parts in enumerate(combined):
    target = runner / f'zz_dod_added_functions_{i}.cpp'
    if target.exists():
        if (backup / target.name).exists():
            raise SystemExit(f'Refusing to overwrite backup: {backup / target.name}')
        shutil.copy2(target, backup / target.name)
    target.write_text('// [DoD] Functions added to the map after the first build (sync_generated_added.py).\n' + '\n'.join(parts))
if added_header.exists():
    shutil.copy2(added_header, backup / added_header.name)
added_header.write_text(header_text)
with report.open('x', encoding='utf-8') as out:
    json.dump({'changed': changed, 'added': added, 'added_declarations': len(added_decls),
               'stub_header_removed_kept': len(stubs_old - stubs_new)}, out, indent=2)
print(f'changed {len(changed)}, added {len(added)}, declarations {len(added_decls)}')

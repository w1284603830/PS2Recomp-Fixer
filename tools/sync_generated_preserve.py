"""Content-only generated code sync; backs up changes and refuses removed files."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import json
import shutil
import argparse
import re

parser = argparse.ArgumentParser(description=__doc__)
for name in ('source', 'runtime', 'backup', 'report'):
    parser.add_argument(name, type=Path)
parser.add_argument('--preserve-redundant-stub-header', action='store_true',
                    help='Keep removed stub declarations only when identical declarations remain in the function header.')
args = parser.parse_args()
source, runtime, backup, report = args.source, args.runtime, args.backup, args.report
if report.exists():
    raise SystemExit(f'Refusing to overwrite report: {report}')
runner = runtime / 'src/runner'
old_names = {p.name for p in runner.glob('*.cpp') if p.name != 'main.cpp'}
new_names = {p.name for p in source.glob('*.cpp')}
if old_names != new_names:
    raise SystemExit(f'File set differs; review before sync: added={new_names-old_names}, removed={old_names-new_names}')
backup.mkdir(parents=True, exist_ok=True)
changed = []
def differs(path):
    destination = runtime / ('include' if path.suffix == '.h' else 'src/runner') / path.name
    return destination.stat().st_size != path.stat().st_size or destination.read_bytes() != path.read_bytes()

files = [p for p in source.iterdir() if p.suffix in ('.cpp', '.h')]
with ThreadPoolExecutor(max_workers=16) as pool:
    different = [p for p, changed_content in zip(files, pool.map(differs, files)) if changed_content]
print(f'Compared {len(files)} files; {len(different)} changes.', flush=True)
preserved = []
if args.preserve_redundant_stub_header:
    header = source / 'ps2_recompiled_stubs.h'
    if header in different:
        normalize = lambda line: re.sub(r'\s+', '', line)
        old_lines = set(filter(None, (normalize(line) for line in (runtime / 'include' / header.name).read_text().splitlines())))
        new_lines = set(filter(None, (normalize(line) for line in header.read_text().splitlines())))
        function_lines = set(normalize(line) for line in (source / 'ps2_recompiled_functions.h').read_text().splitlines())
        removed = old_lines - new_lines
        if new_lines - old_lines or any(not line.startswith('voidFUN_') or line not in function_lines for line in removed):
            raise SystemExit('Stub header changes are not solely redundant removed declarations; review required.')
        different.remove(header)
        preserved.append({'file': header.name, 'redundant_declarations': len(removed)})
for path in different:
    if (backup / path.name).exists():
        raise SystemExit(f'Refusing to overwrite backup: {backup / path.name}')
for path in different:
    destination = runtime / ('include' if path.suffix == '.h' else 'src/runner') / path.name
    saved = backup / path.name
    if saved.exists():
        raise SystemExit(f'Refusing to overwrite backup: {saved}')
    shutil.copy2(destination, saved)
    shutil.copy2(path, destination)
    changed.append(path.name)
with report.open('x', encoding='utf-8') as output:
    json.dump({'changed': changed, 'preserved_headers': preserved}, output, indent=2)
print(f'Changed {len(changed)} files: {changed}')

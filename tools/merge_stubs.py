"""Merge the SDK stubs found by ps2_analyzer (SCE symbol database) into the Ghidra-exported TOML.

The Ghidra TOML keeps its function map (ghidra_output) and settings; its stubs list becomes the union of both
lists, keyed by address (the analyzer's name wins when both name the same address). The analyzer's suggested
instruction patches are NOT copied: they NOP out code it only suspects is self-modifying.

Usage: python -I merge_stubs.py <ghidra.toml> <analyzer.toml> <out.toml>
"""
import re
import sys

ARRAY = re.compile(r"^stubs\s*=\s*\[(.*?)^\]", re.M | re.S)
ITEM = re.compile(r'"([^"@]+)@0x([0-9A-Fa-f]+)"')


def stubs(text):
    m = ARRAY.search(text)
    if not m:
        raise SystemExit("no stubs array")
    return {int(a, 16): n for n, a in ITEM.findall(m.group(1))}


def main():
    ghidra_path, analyzer_path, out_path = sys.argv[1:4]
    ghidra = open(ghidra_path, encoding="utf-8").read()
    analyzer = open(analyzer_path, encoding="utf-8").read()
    merged = stubs(ghidra)
    found = stubs(analyzer)
    renamed = sum(1 for a, n in found.items() if a in merged and merged[a] != n)
    added = sum(1 for a in found if a not in merged)
    merged.update(found)
    body = "".join('  "%s@0x%08X",\n' % (merged[a], a) for a in sorted(merged))
    out = ARRAY.sub(lambda m: "stubs = [\n" + body + "]", ghidra, count=1)
    open(out_path, "w", encoding="utf-8", newline="\n").write(out)
    print("ghidra stubs %d, analyzer stubs %d, added %d, renamed %d, total %d"
          % (len(stubs(ghidra)), len(found), added, renamed, len(merged)))


if __name__ == "__main__":
    main()

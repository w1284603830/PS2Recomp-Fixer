"""Copy recompiler output into the runtime tree, touching only files whose content changed.

Generated .cpp files go to ps2xRuntime/src/runner, generated headers to ps2xRuntime/include (as
ps2xRuntime/Readme.md describes). Unchanged files keep their timestamps, so MSBuild recompiles only what
changed; generated files that no longer exist are removed. Only files that look generated are ever removed (the
patterns .gitignore lists: *_0x*.cpp, zz_*_added_functions_*.cpp); your own main.cpp, register_functions.cpp and any
hand-written .cpp in src/runner are kept and reported.

Usage: python -I sync_generated.py [--dry-run] <recomp output dir> [<ps2xRuntime dir>]
  --dry-run   say what would be copied and removed, change nothing
"""
import fnmatch
import filecmp
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


GENERATED_PATTERNS = ("*_0x*.cpp", "zz_*_added_functions_*.cpp")


def looks_generated(name):
    return any(fnmatch.fnmatch(name, p) for p in GENERATED_PATTERNS)


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = len(args) != len(sys.argv) - 1
    if not args:
        raise SystemExit(__doc__)
    out = args[0]
    runtime = args[1] if len(args) > 1 else os.path.join(HERE, "..", "third_party", "src", "PS2Recomp", "ps2xRuntime")
    runner = os.path.join(runtime, "src", "runner")
    include = os.path.join(runtime, "include")
    names = os.listdir(out)
    if "main.cpp" in names:
        raise SystemExit("recompiler output unexpectedly contains main.cpp")
    cpp = {n for n in names if n.endswith(".cpp")}
    hdr = [n for n in names if n.endswith(".h")]

    copied = same = removed = 0
    kept = []
    for n in sorted(cpp):
        src, dst = os.path.join(out, n), os.path.join(runner, n)
        if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
            same += 1
            continue
        if not dry:
            shutil.copyfile(src, dst)
        copied += 1
    for n in os.listdir(runner):
        if n.endswith(".cpp") and n != "main.cpp" and n not in cpp:
            if looks_generated(n):
                if not dry:
                    os.remove(os.path.join(runner, n))
                removed += 1
            else:
                kept.append(n)
    for n in hdr:
        src, dst = os.path.join(out, n), os.path.join(include, n)
        if not (os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False)):
            if not dry:
                shutil.copyfile(src, dst)
            copied += 1
    if kept:
        print("kept (not generated, not in the output): " + ", ".join(sorted(kept)))
    print("[dry run] " if dry else "", end="")
    print("generated .cpp %d: copied/changed %d, unchanged %d, removed %d; headers %s" % (len(cpp), copied, same, removed, ", ".join(hdr)))


if __name__ == "__main__":
    main()

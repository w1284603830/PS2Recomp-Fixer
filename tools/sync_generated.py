"""Copy recompiler output into the runtime tree, touching only files whose content changed.

Generated .cpp files go to ps2xRuntime/src/runner, generated headers to ps2xRuntime/include (as
ps2xRuntime/Readme.md describes). Unchanged files keep their timestamps, so MSBuild recompiles only what
changed; generated files that no longer exist are removed. Our host entry point src/runner/main.cpp is kept.

Usage: python -I sync_generated.py <recomp output dir> [<ps2xRuntime dir>]
"""
import filecmp
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    out = sys.argv[1]
    runtime = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "third_party", "src", "PS2Recomp", "ps2xRuntime")
    runner = os.path.join(runtime, "src", "runner")
    include = os.path.join(runtime, "include")
    names = os.listdir(out)
    if "main.cpp" in names:
        raise SystemExit("recompiler output unexpectedly contains main.cpp")
    cpp = {n for n in names if n.endswith(".cpp")}
    hdr = [n for n in names if n.endswith(".h")]

    copied = same = removed = 0
    for n in sorted(cpp):
        src, dst = os.path.join(out, n), os.path.join(runner, n)
        if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
            same += 1
            continue
        shutil.copyfile(src, dst)
        copied += 1
    for n in os.listdir(runner):
        if n.endswith(".cpp") and n != "main.cpp" and n not in cpp:
            os.remove(os.path.join(runner, n))
            removed += 1
    for n in hdr:
        src, dst = os.path.join(out, n), os.path.join(include, n)
        if not (os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False)):
            shutil.copyfile(src, dst)
            copied += 1
    print("generated .cpp %d: copied/changed %d, unchanged %d, removed %d; headers %s" % (len(cpp), copied, same, removed, ", ".join(hdr)))


if __name__ == "__main__":
    main()

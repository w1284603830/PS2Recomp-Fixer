"""Copy recompiler output into the runtime tree, touching only files whose content changed.

Generated .cpp files go to ps2xRuntime/src/runner, generated headers to ps2xRuntime/include (as
ps2xRuntime/Readme.md describes). Unchanged files keep their timestamps, so MSBuild recompiles only what
changed; generated files that no longer exist are removed. Only files that look generated are ever removed (the
patterns .gitignore lists: *_0x*.cpp, zz_*_added_functions_*.cpp); your own main.cpp, register_functions.cpp and any
hand-written .cpp in src/runner are kept and reported.
The output must contain register_functions.cpp and both ps2_recompiled_functions.h / ps2_recompiled_stubs.h;
incomplete output and directories overlapping the destination are rejected before changing the runtime.

Usage: python -I sync_generated.py [--dry-run] <recomp output dir> [<ps2xRuntime dir>]
  --dry-run   say what would be copied and removed, change nothing
"""
import argparse
import fnmatch
import filecmp
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))


GENERATED_PATTERNS = ("*_0x*.cpp", "zz_*_added_functions_*.cpp")


def looks_generated(name):
    return any(fnmatch.fnmatch(name, p) for p in GENERATED_PATTERNS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("out")
    parser.add_argument("runtime", nargs="?", default=os.path.join(HERE, "..", "third_party", "src", "PS2Recomp", "ps2xRuntime"))
    args = parser.parse_args()
    out, runtime, dry = args.out, args.runtime, args.dry_run
    runner = os.path.join(runtime, "src", "runner")
    include = os.path.join(runtime, "include")
    # Validate the complete output before copying or pruning anything. An empty or interrupted
    # recompile must never be interpreted as a request to delete the existing generated code.
    for target in (runner, include):
        source_real, target_real = os.path.realpath(out), os.path.realpath(target)
        try:
            overlap = os.path.commonpath((source_real, target_real)) in (source_real, target_real)
        except ValueError:  # Different Windows drives cannot overlap.
            overlap = False
        if overlap:
            raise SystemExit("recompiler output must not overlap the runtime runner or include directories")
    names = os.listdir(out)
    if "main.cpp" in names:
        raise SystemExit("recompiler output unexpectedly contains main.cpp")
    cpp = {n for n in names if n.endswith(".cpp")}
    hdr = sorted(n for n in names if n.endswith(".h"))
    required = {"register_functions.cpp", "ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"}
    missing = required - set(names)
    if missing:
        raise SystemExit("incomplete recompiler output; missing: " + ", ".join(sorted(missing)))
    if any(not os.path.isfile(os.path.join(out, n)) for n in cpp | set(hdr)):
        raise SystemExit("recompiler output contains a non-file .cpp or .h entry")
    if not dry:
        os.makedirs(runner, exist_ok=True)
        os.makedirs(include, exist_ok=True)

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
    for n in os.listdir(runner) if os.path.isdir(runner) else []:
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

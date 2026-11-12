"""Start a new port: take a PS2 disc image (or boot ELF) you own and lay out a port folder outside this repository.

    python -I tools/new_port.py --iso D:\\games\\MyGame.iso --out D:\\ports\\mygame [--title "My Game"]
    python -I tools/new_port.py --elf D:\\dump\\SLUS_123.45 --out D:\\ports\\mygame

What it does (read-only on the disc image; nothing is written inside this repository):
  1. reads SYSTEM.CNF from the ISO9660 image and copies the boot ELF it names (BOOT2) to <out>/game/,
     after checking that it is a 32-bit little-endian MIPS ELF;
  2. writes <out>/recomp.toml for ps2_recomp: with ps2_analyzer (found in build/ or given with --analyzer) the
     analyzer's full configuration, otherwise a minimal one to complete by hand;
  3. writes <out>/game.ini for examples/generic_runner, <out>/port.json (serial, ELF SHA-1) and
     <out>/PORT_STEPS.md with the exact commands for the remaining steps.

Game files stay in <out>, which must be outside this repository (game data must never be committed).
Exit status: 0 done, 2 bad arguments or input, 3 output folder refused.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SECTOR = 2048


class PortError(Exception):
    def __init__(self, message, code=2):
        super().__init__(message)
        self.code = code


def _iso_walk():
    # iso_extract.py's directory walker. Loaded by path: under `python -I` the script folder is not on sys.path.
    spec = importlib.util.spec_from_file_location("ps2fixer_iso_extract", HERE / "iso_extract.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.walk


def iso_files(iso):
    """{upper-case path: (lba, size)} of every file in the image."""
    with open(iso, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if len(pvd) < 190 or pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise PortError(f"{iso}: no ISO9660 primary volume descriptor at sector 16 (a .bin/.chd must be "
                            "converted to a plain .iso first)")
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        out = []
        try:
            _iso_walk()(f, lba, size, "", out)
        except (UnicodeDecodeError, struct.error, IndexError) as error:
            raise PortError(f"{iso}: unreadable ISO9660 directory ({error})")
    return {path.upper(): (lba, size) for path, lba, size in out}


def read_iso_file(iso, entry):
    lba, size = entry
    with open(iso, "rb") as f:
        f.seek(lba * SECTOR)
        data = f.read(size)
    if len(data) != size:
        raise PortError(f"{iso}: file extent past the end of the image")
    return data


def boot_path(system_cnf):
    """'cdrom0:\\SLUS_123.45;1' -> 'SLUS_123.45' (an ISO path, '/' separated, upper case)."""
    text = system_cnf.decode("ascii", "replace")
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key.strip().upper() == "BOOT2":
            value = value.strip()
            if not value.lower().startswith("cdrom0:"):
                raise PortError(f"SYSTEM.CNF BOOT2 is not on the disc: {value!r}")
            path = value[len("cdrom0:"):].lstrip("\\/").split(";")[0].replace("\\", "/")
            if not path or ".." in path.split("/"):
                raise PortError(f"SYSTEM.CNF BOOT2 has no usable path: {value!r}")
            return path.upper()
    raise PortError("SYSTEM.CNF has no BOOT2 line (a PS1 disc, or not a game disc)")


def check_elf(data, name):
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise PortError(f"{name}: not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise PortError(f"{name}: not a 32-bit little-endian ELF (the EE's format)")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != 8:
        raise PortError(f"{name}: ELF machine {machine}, not MIPS (8)")
    return struct.unpack_from("<I", data, 24)[0]  # entry point


def serial_of(elf_name):
    stem = Path(elf_name).name.upper()
    letters, _, digits = stem.partition("_")
    digits = digits.replace(".", "")
    if len(letters) == 4 and letters.isalpha() and digits.isdigit():
        return f"{letters}-{digits}"
    return stem


def inside(path, parent):
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def find_analyzer(explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise PortError(f"--analyzer {explicit}: no such file")
        return path
    for candidate in (ROOT / "build/ps2recomp/ps2xAnalyzer/Release/ps2_analyzer.exe",
                      ROOT / "build/ps2recomp/ps2xAnalyzer/ps2_analyzer",
                      ROOT / "build/ps2recomp/ps2xAnalyzer/Release/ps2_analyzer"):
        if candidate.is_file():
            return candidate
    found = shutil.which("ps2_analyzer")
    return Path(found) if found else None


def toml_string(value):
    return json.dumps(str(value))  # a JSON string is a valid TOML basic string


MINIMAL_TOML = """# Minimal ps2_recomp configuration written by tools/new_port.py (ps2_analyzer was not available).
# Run ps2_analyzer on the ELF for a complete one: it finds the SDK functions to stub or skip from the ELF's own
# symbols and the embedded SCE symbol database.
[general]
input = {elf}
ghidra_output = ""
output = {output}
single_file_output = false
patch_syscalls = false
patch_cop0 = true
patch_cache = true
# Functions to leave out, replace with the runtime's HLE, or force back to recompiled guest code. Selectors are
# "name", "name@0xADDRESS" or "0xADDRESS". `native` takes address selectors only: it keeps a function whose name
# matches an SDK call (for example a game's own SetEventFlag) as recompiled code instead of binding it to the HLE.
skip = []
stubs = []
native = []
entry_points = []
"""

NATIVE_NOTE = """
# Added by tools/new_port.py: if a game function shares its name with an SDK call and is wrongly replaced by the
# runtime's HLE, list its address in [general] native = ["name@0xADDRESS"] to keep the original recompiled code.
"""


def write_steps(out, info, analyzer_used):
    repo = ROOT
    runtime = repo / "third_party/src/PS2Recomp/ps2xRuntime"
    exe_dir = repo / "build/ps2recomp/ps2xRuntime/Release"
    recomp = repo / "build/ps2recomp/ps2xRecomp/Release/ps2_recomp.exe"
    analyzer = repo / "build/ps2recomp/ps2xAnalyzer/Release/ps2_analyzer.exe"
    lines = [
        f"# Port of {info['title']} ({info['serial']})",
        "",
        f"Boot ELF `{info['elf']}` (SHA-1 `{info['elf_sha1']}`, entry `0x{info['entry']:08X}`), created by "
        "`tools/new_port.py`. Game files stay in this folder; never copy them into the repository.",
        "",
        "## Steps",
        "",
        "1. Build the tools once (PowerShell, in the repository): `powershell -NoProfile -ExecutionPolicy Bypass -File "
        f"\"{repo / 'build_all.ps1'}\"` (`fetch_deps.ps1` first in a fresh clone). Add `-CheatMenu` / `-Trainer` only "
        "if this port wants the optional F2 cheat menu / F3 trainer (both off by default).",
    ]
    if not analyzer_used:
        lines.append(f"2. Generate the full configuration: `\"{analyzer}\" \"{out / 'game' / info['elf']}\" "
                     f"\"{out / 'recomp.toml'}\"` (this replaces the minimal recomp.toml).")
    else:
        lines.append("2. `recomp.toml` was generated by ps2_analyzer; review its `skip` / `stubs` lists.")
    lines += [
        f"3. Recompile: `\"{recomp}\" \"{out / 'recomp.toml'}\"` (output in `{out / 'generated'}`).",
        f"4. Copy the output into the runtime: `python -I \"{repo / 'tools/sync_generated.py'}\" \"{out / 'generated'}\" "
        f"\"{runtime}\"`.",
        f"5. Copy `{repo / 'examples/generic_runner/main.cpp'}` to `{runtime / 'src/runner/main.cpp'}` (once).",
        "6. Build again (step 1) and copy `game.ini` from this folder next to "
        f"`{exe_dir / 'ps2EntryRunner.exe'}`, then run it.",
        "",
        "## When something goes wrong",
        "",
        "- A game function bound to the HLE because its name matches an SDK call: add its address to `native` in "
        "`recomp.toml` (`\"name@0xADDRESS\"`) and repeat steps 3-6.",
        "- Missing functions or indirect-jump targets: add `entry_points` hints; see `docs/GAME_PORTING.md` and "
        "`docs/REGRESSION_TESTING.md` for the per-game values (GS options, IPU/MPEG/font profiles) and test workflow.",
        "",
    ]
    (out / "PORT_STEPS.md").write_text("\n".join(lines), encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Lay out a new port folder from a disc image or boot ELF you own.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--iso", help="plain ISO9660 disc image (.iso)")
    source.add_argument("--elf", help="boot ELF already copied from the disc")
    parser.add_argument("--out", required=True, help="new port folder (outside this repository)")
    parser.add_argument("--title", help="window title (default: the disc serial)")
    parser.add_argument("--analyzer", help="path to ps2_analyzer (default: build/ output, then PATH)")
    parser.add_argument("--no-analyzer", action="store_true", help="write the minimal recomp.toml only")
    parser.add_argument("--force", action="store_true", help="allow a non-empty output folder (files are replaced)")
    args = parser.parse_args(argv)

    try:
        out = Path(args.out)
        if inside(out, ROOT):
            raise PortError(f"--out {out} is inside this repository; game files must stay outside it", 3)
        if out.exists() and any(out.iterdir()) and not args.force:
            raise PortError(f"--out {out} is not empty (use --force to replace the files new_port writes)", 3)

        iso = None
        if args.iso:
            iso = Path(args.iso).resolve()
            if not iso.is_file():
                raise PortError(f"--iso {args.iso}: no such file")
            files = iso_files(iso)
            if "SYSTEM.CNF" not in files:
                raise PortError(f"{iso}: no SYSTEM.CNF in the root directory")
            elf_path = boot_path(read_iso_file(iso, files["SYSTEM.CNF"]))
            if elf_path not in files:
                raise PortError(f"{iso}: SYSTEM.CNF names {elf_path}, which is not on the disc")
            elf_data = read_iso_file(iso, files[elf_path])
            elf_name = Path(elf_path).name
        else:
            source_elf = Path(args.elf)
            if not source_elf.is_file():
                raise PortError(f"--elf {args.elf}: no such file")
            elf_data = source_elf.read_bytes()
            elf_name = source_elf.name
        entry = check_elf(elf_data, elf_name)

        analyzer = None if args.no_analyzer else find_analyzer(args.analyzer)
        (out / "game").mkdir(parents=True, exist_ok=True)
        elf_out = out / "game" / elf_name
        elf_out.write_bytes(elf_data)
        serial = serial_of(elf_name)
        info = {
            "serial": serial,
            "title": args.title or serial,
            "elf": elf_name,
            "elf_sha1": hashlib.sha1(elf_data).hexdigest(),
            "entry": entry,
            "iso": str(iso) if iso else None,
            "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "created_by": "PS2Recomp-Fixer tools/new_port.py",
        }

        toml = out / "recomp.toml"
        analyzer_used = False
        if analyzer:
            result = subprocess.run([str(analyzer), str(elf_out), str(toml)], capture_output=True, text=True,
                                    timeout=1800)
            analyzer_used = result.returncode == 0 and toml.is_file()
            if analyzer_used:
                text = toml.read_text(encoding="utf-8", errors="replace")
                if "native" not in text:
                    toml.write_text(text.rstrip("\n") + "\n" + NATIVE_NOTE, encoding="utf-8")
            else:
                print(f"[new_port] ps2_analyzer failed (exit {result.returncode}); writing the minimal recomp.toml",
                      file=sys.stderr)
                print(result.stderr[-2000:], file=sys.stderr)
        if not analyzer_used:
            toml.write_text(MINIMAL_TOML.format(elf=toml_string(elf_out.as_posix()),
                                                output=toml_string((out / "generated").as_posix())), encoding="utf-8")

        ini = [f"# examples/generic_runner settings for {serial}, written by tools/new_port.py",
               f"elf = {elf_out}"]
        if iso:
            ini.append(f"iso = {iso}")
        else:
            ini.append("# iso = <your disc image>   (required: the game reads its files from it)")
        ini += [f"title = {info['title']}", "# window = 1280x960", "# gs_options =   (see docs/GAME_PORTING.md)", ""]
        (out / "game.ini").write_text("\n".join(ini), encoding="utf-8")
        (out / "port.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
        write_steps(out.resolve(), info, analyzer_used)
    except PortError as error:
        print(f"[new_port] {error}", file=sys.stderr)
        return error.code
    except OSError as error:
        print(f"[new_port] {error}", file=sys.stderr)
        return 2

    print(f"[new_port] {serial}: {elf_name} (entry 0x{entry:08X}) -> {out}")
    print(f"[new_port] recomp.toml from {'ps2_analyzer' if analyzer_used else 'the minimal template'}; "
          f"next steps in {out / 'PORT_STEPS.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

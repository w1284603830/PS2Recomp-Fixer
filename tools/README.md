# Tool boundaries

`new_port.py` starts a port: from a disc image or boot ELF it lays out a port folder outside this repository (boot ELF
from SYSTEM.CNF, `recomp.toml`, `game.ini`, `port.json`, `PORT_STEPS.md`). It reads the image only and refuses an
output folder inside the repository. Tests: `tests/test_new_port.py`.

General tools in this directory operate on caller-supplied source, maps, captures or archives. Hardware address
labels in GIF/IO census tools describe PS2 registers. They are not a game's RAM layout. Run tools on copies and
review generated function-map changes before rebuilding a port.

The following fixed-range/counter scripts moved to `examples/dod_runner/tools/`, preserving the relative `diag/`
subdirectory. Their constants describe the reference game's executable and are not generic defaults:

- `fix_function_map.py`, `scan_code_pointers.py`, `scan_jump_tables.py`, `find_missing_function_starts.py`
- `diag/cop2_census.py`, `diag/mmi_census.py`, `diag/find_const.py`, `diag/find_io.py`, `diag/find_io2.py`
- `diag/hle_inventory.py`, `diag/pcsx2_compare_report.py`

These scripts remain historical reference tools, with their original bounds/assumptions. Other games need ELF segment
mapping, executable/data ranges, instruction/ABI guards and measured counters from their own revision. No renamed
script is claimed to implement a general ELF analysis. `tools/diag/ee_dis.py` already reads ELF load segments;
`tools/diag/mips_dis.py` takes an explicit module load base. Library-prefix inventory and capture analyzers remain
shared where their inputs determine the addresses.

For failure triage, capture consumed input and the first differing EE/VU/GIF state, then use
[REGRESSION_TESTING.md](../docs/REGRESSION_TESTING.md). Opcode counts, missing-map counts or an empty smoke runner
cannot establish full-game correctness.

## Performance and compatibility (`tools/perf/`)

Game-neutral, Windows x64, read-only except `cpu_cap.py` (a Job Object CPU cap on the process it is given). See
[docs/CROSS_DEVICE_OPTIMIZATION.md](../docs/CROSS_DEVICE_OPTIMIZATION.md).

- `thread_cpu_sample.py`: CPU % per named thread per interval (mean / p95 / max).
- `thread_profile.py`: RIP sampling of one named thread, symbolized with the EXE `/MAP` (and `--map` for DLLs).
- `cpu_cap.py`: weak-host simulation on a schedule (attach to a running port by process id).
- `isa_scan.py`, `frame_sizes.py`: AVX/AVX2/FMA/BMI instruction counts and per-function stack frames (dumpbin; set
  `DUMPBIN` or use a Visual Studio developer prompt).
